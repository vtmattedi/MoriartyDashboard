#include "Ota.h"
#include "Battery.h"

#include <NightMare.h>
#include <Version.h>

#include <atomic>
#include <errno.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

// The pairing token lives in include/creds.h with the other secrets. Without it
// update mode refuses to open: an update server anyone on the LAN can talk to
// is not one to ship.
#if __has_include("creds.h")
#include "creds.h"
#endif
#ifndef DASHLINK_TOKEN
#define DASHLINK_TOKEN ""
#endif

// See Ota.h for what this is, and Dash-P4/pc/PROTOCOL.md for the wire format.

namespace
{
    constexpr uint16_t LinkPort = 7340;
    constexpr uint32_t HeartbeatSeconds = 5;
    // The PC pings every heartbeat; three missed ones and the link is dead.
    constexpr uint32_t SilenceLimitMs = HeartbeatSeconds * 3 * 1000;
    constexpr uint32_t HelloWaitMs = 5000;
    // How long update mode stays open with nobody using it.
    constexpr uint32_t IdleCloseMs = 120000;
    // A new image is called good this long after boot ...
    constexpr uint32_t MarkValidAfterMs = 20000;
    // ... and the listener a freshly updated panel opens for the PC's check
    // stays up this long after boot, well past that, so the PC's reconnect
    // backoff has room.
    constexpr uint32_t ProbationListenMs = 60000;
    constexpr size_t MaxFrame = 16384; // the protocol's limit on one frame
    constexpr size_t JsonCap = 1024;   // the largest JSON body this side reads
    constexpr size_t ChunkCap = 2048;  // image bytes are streamed through this

    std::atomic<uint8_t> gState{(uint8_t)OtaState::Off};
    std::atomic<uint8_t> gPercent{0};
    std::atomic<uint32_t> gRevision{0};
    std::atomic<bool> gStop{false};
    std::atomic<uint32_t> gCloseAtMs{0};
    // Always a string literal, so reading it from another task is safe.
    std::atomic<const char *> gReason{nullptr};

    bool pendingVerify = false; // this boot is a fresh image on probation
    bool marked = false;

    void setState(OtaState state)
    {
        gState.store((uint8_t)state);
        gRevision.fetch_add(1);
    }

    void setPercent(uint8_t percent)
    {
        if (gPercent.exchange(percent) != percent)
        {
            gRevision.fetch_add(1);
        }
    }

    void fail(const char *reason)
    {
        gReason.store(reason);
        setState(OtaState::Failed);
        printf("[ota] failed: %s\n", reason);
    }

#pragma region "Socket helpers"

    /// Read exactly n bytes. The socket has a one-second receive timeout so
    /// that a stop request is noticed; `limitMs` is how long a silent peer is
    /// tolerated.
    bool readExact(int fd, uint8_t *dst, size_t n, uint32_t limitMs)
    {
        uint32_t lastProgress = millisNow();
        while (n > 0)
        {
            if (gStop.load())
            {
                return false;
            }
            const int got = recv(fd, dst, n, 0);
            if (got > 0)
            {
                dst += got;
                n -= (size_t)got;
                lastProgress = millisNow();
            }
            else if (got == 0)
            {
                return false; // closed
            }
            else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                if ((millisNow() - lastProgress) >= limitMs)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    bool sendAll(int fd, const uint8_t *src, size_t n)
    {
        while (n > 0)
        {
            const int sent = send(fd, src, n, 0);
            if (sent <= 0)
            {
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                {
                    continue; // SO_SNDTIMEO bounds each wait; gStop ends the loop
                }
                return false;
            }
            src += sent;
            n -= (size_t)sent;
        }
        return true;
    }

    /// One frame: u32 LE length (type byte + body), the type letter, the JSON.
    bool sendFrame(int fd, char kind, const char *json)
    {
        const size_t bodyLen = json ? strlen(json) : 0;
        uint8_t head[5];
        const uint32_t length = (uint32_t)(1 + bodyLen);
        head[0] = (uint8_t)(length);
        head[1] = (uint8_t)(length >> 8);
        head[2] = (uint8_t)(length >> 16);
        head[3] = (uint8_t)(length >> 24);
        head[4] = (uint8_t)kind;
        return sendAll(fd, head, sizeof(head)) &&
               (bodyLen == 0 || sendAll(fd, (const uint8_t *)json, bodyLen));
    }

    struct Frame
    {
        char kind = 0;
        uint32_t payload = 0; // bytes of the body still unread
    };

    bool readHeader(int fd, Frame &frame, uint32_t limitMs)
    {
        uint8_t head[5];
        if (!readExact(fd, head, sizeof(head), limitMs))
        {
            return false;
        }
        const uint32_t length = head[0] | (head[1] << 8) | (head[2] << 16) | ((uint32_t)head[3] << 24);
        if (length < 1 || length > MaxFrame)
        {
            return false;
        }
        frame.kind = (char)head[4];
        frame.payload = length - 1;
        return true;
    }

    bool discard(int fd, Frame &frame)
    {
        uint8_t scratch[128];
        while (frame.payload > 0)
        {
            const size_t n = frame.payload < sizeof(scratch) ? frame.payload : sizeof(scratch);
            if (!readExact(fd, scratch, n, SilenceLimitMs))
            {
                return false;
            }
            frame.payload -= (uint32_t)n;
        }
        return true;
    }

    /// Read a JSON body into `out`. False means the link is gone; a body too
    /// big to keep is consumed and comes back empty, which every caller treats
    /// as malformed.
    bool readJson(int fd, Frame &frame, char *out, size_t cap)
    {
        out[0] = '\0';
        if (frame.payload >= cap)
        {
            return discard(fd, frame);
        }
        if (!readExact(fd, (uint8_t *)out, frame.payload, SilenceLimitMs))
        {
            return false;
        }
        out[frame.payload] = '\0';
        frame.payload = 0;
        return true;
    }

#pragma endregion

#pragma region "Reading the few JSON fields this needs"

    // The bodies are flat objects of strings and numbers written by dashlink.
    // A scan for the key is enough, and anything unexpected fails the check
    // that follows (the HMAC, the size, the hash) rather than being trusted.

    const char *findValue(const char *json, const char *key)
    {
        char pattern[24];
        snprintf(pattern, sizeof(pattern), "\"%s\"", key);
        const char *at = strstr(json, pattern);
        if (!at)
        {
            return nullptr;
        }
        at += strlen(pattern);
        while (*at == ' ')
        {
            ++at;
        }
        if (*at != ':')
        {
            return nullptr;
        }
        ++at;
        while (*at == ' ')
        {
            ++at;
        }
        return at;
    }

    bool jsonString(const char *json, const char *key, char *out, size_t cap)
    {
        const char *at = findValue(json, key);
        if (!at || *at != '"')
        {
            return false;
        }
        ++at;
        size_t n = 0;
        while (*at && *at != '"')
        {
            char c = *at++;
            if (c == '\\')
            {
                switch (*at++)
                {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                default: return false; // \uXXXX and the rest: not worth supporting; the proof would not match
                }
            }
            if (n + 1 >= cap)
            {
                return false;
            }
            out[n++] = c;
        }
        if (*at != '"')
        {
            return false;
        }
        out[n] = '\0';
        return true;
    }

    bool jsonNumber(const char *json, const char *key, uint32_t &out)
    {
        const char *at = findValue(json, key);
        if (!at || *at < '0' || *at > '9')
        {
            return false;
        }
        out = (uint32_t)strtoul(at, nullptr, 10);
        return true;
    }

#pragma endregion

#pragma region "Crypto"

    void toHex(const uint8_t *bytes, size_t n, char *out)
    {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < n; ++i)
        {
            out[2 * i] = digits[bytes[i] >> 4];
            out[2 * i + 1] = digits[bytes[i] & 15];
        }
        out[2 * n] = '\0';
    }

    bool sameText(const char *a, const char *b)
    {
        const size_t n = strlen(a);
        if (n != strlen(b))
        {
            return false;
        }
        uint8_t diff = 0;
        for (size_t i = 0; i < n; ++i)
        {
            diff |= (uint8_t)(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    /// hex(HMAC-SHA256(key = token, message = nonce + "|" + name)), as dashlink computes it.
    bool proofFor(const char *nonce, const char *name, char out[65])
    {
        const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        uint8_t digest[32];
        bool ok = info && mbedtls_md_setup(&ctx, info, 1) == 0 &&
                  mbedtls_md_hmac_starts(&ctx, (const uint8_t *)DASHLINK_TOKEN, strlen(DASHLINK_TOKEN)) == 0 &&
                  mbedtls_md_hmac_update(&ctx, (const uint8_t *)nonce, strlen(nonce)) == 0 &&
                  mbedtls_md_hmac_update(&ctx, (const uint8_t *)"|", 1) == 0 &&
                  mbedtls_md_hmac_update(&ctx, (const uint8_t *)name, strlen(name)) == 0 &&
                  mbedtls_md_hmac_finish(&ctx, digest) == 0;
        mbedtls_md_free(&ctx);
        if (ok)
        {
            toHex(digest, sizeof(digest), out);
        }
        return ok;
    }

#pragma endregion

    /// Reject an update request; the session carries on, the PC decides what next.
    void reject(int fd, const char *why)
    {
        char body[160];
        snprintf(body, sizeof(body), "{\"state\":\"failed\",\"pct\":0,\"msg\":\"%s\"}", why);
        sendFrame(fd, 'T', body);
        fail(why);
    }

    void sendStatus(int fd, const char *state, uint8_t percent)
    {
        char body[64];
        snprintf(body, sizeof(body), "{\"state\":\"%s\",\"pct\":%u}", state, (unsigned)percent);
        sendFrame(fd, 'T', body);
    }

    void answerPing(int fd, Frame &frame)
    {
        char json[64];
        uint32_t t = 0;
        if (readJson(fd, frame, json, sizeof(json)))
        {
            jsonNumber(json, "t", t);
            char body[40];
            snprintf(body, sizeof(body), "{\"t\":%lu}", (unsigned long)t);
            sendFrame(fd, 'O', body);
        }
    }

    /// The image transfer, from the `U` frame to the restart. Returns true
    /// when the panel is about to restart; false ends the session (the
    /// reason has been sent and recorded).
    bool receiveImage(int fd, Frame &frame)
    {
        char json[JsonCap];
        if (!readJson(fd, frame, json, sizeof(json)))
        {
            return false;
        }

        char target[16] = "";
        char wantHash[65] = "";
        uint32_t size = 0;
        jsonString(json, "target", target, sizeof(target));
        jsonString(json, "sha256", wantHash, sizeof(wantHash));
        jsonNumber(json, "size", size);

        if (strcmp(target, "dashboard") != 0)
        {
            reject(fd, "this panel takes target dashboard");
            return false;
        }
        if (strlen(wantHash) != 64)
        {
            reject(fd, "the request has no sha256");
            return false;
        }
        if (Battery_present() && Battery_percent() < OTA_MIN_BATTERY_PERCENT)
        {
            reject(fd, "battery too low to update safely");
            return false;
        }
        const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
        if (!slot)
        {
            reject(fd, "no update slot");
            return false;
        }
        if (size < 65536 || size > slot->size)
        {
            reject(fd, "the image does not fit the update slot");
            return false;
        }

        esp_ota_handle_t handle = 0;
        // Erase as the writes go rather than the whole slot up front: that is
        // several seconds the PC would wait through before "ready".
        if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK)
        {
            reject(fd, "could not open the update slot");
            return false;
        }
        mbedtls_sha256_context hash;
        mbedtls_sha256_init(&hash);
        mbedtls_sha256_starts(&hash, 0);

        printf("[ota] receiving %lu bytes into %s\n", (unsigned long)size, slot->label);
        setPercent(0);
        setState(OtaState::Receiving);
        gReason.store(nullptr);
        sendStatus(fd, "ready", 0);

        const auto abortWith = [&](const char *why)
        {
            esp_ota_abort(handle);
            mbedtls_sha256_free(&hash);
            char body[160];
            snprintf(body, sizeof(body), "{\"state\":\"failed\",\"pct\":%u,\"msg\":\"%s\"}",
                     (unsigned)gPercent.load(), why);
            sendFrame(fd, 'T', body);
            fail(why);
            return false;
        };

        uint8_t *chunk = (uint8_t *)malloc(ChunkCap);
        if (!chunk)
        {
            return abortWith("out of memory");
        }
        uint32_t received = 0;
        uint8_t lastTenth = 0;
        while (received < size)
        {
            if (!readHeader(fd, frame, SilenceLimitMs))
            {
                free(chunk);
                return abortWith("the link dropped during the transfer");
            }
            if (frame.kind == 'P')
            {
                answerPing(fd, frame);
                continue;
            }
            if (frame.kind != 'D')
            {
                if (!discard(fd, frame))
                {
                    free(chunk);
                    return abortWith("the link dropped during the transfer");
                }
                continue;
            }
            if (frame.payload > size - received)
            {
                free(chunk);
                return abortWith("more data than announced");
            }
            while (frame.payload > 0)
            {
                const size_t n = frame.payload < ChunkCap ? frame.payload : ChunkCap;
                if (!readExact(fd, chunk, n, SilenceLimitMs))
                {
                    free(chunk);
                    return abortWith("the link dropped during the transfer");
                }
                if (esp_ota_write(handle, chunk, n) != ESP_OK)
                {
                    free(chunk);
                    return abortWith("flash write failed");
                }
                mbedtls_sha256_update(&hash, chunk, n);
                received += (uint32_t)n;
                frame.payload -= (uint32_t)n;
            }
            const uint8_t percent = (uint8_t)((uint64_t)received * 100 / size);
            setPercent(percent);
            if (percent / 10 != lastTenth)
            {
                lastTenth = percent / 10;
                sendStatus(fd, "running", percent);
            }
        }
        free(chunk);

        setState(OtaState::Verifying);
        uint8_t digest[32];
        mbedtls_sha256_finish(&hash, digest);
        mbedtls_sha256_free(&hash);
        char gotHash[65];
        toHex(digest, sizeof(digest), gotHash);
        if (!sameText(gotHash, wantHash))
        {
            esp_ota_abort(handle);
            char body[96];
            snprintf(body, sizeof(body), "{\"state\":\"failed\",\"pct\":100,\"msg\":\"checksum mismatch\"}");
            sendFrame(fd, 'T', body);
            fail("checksum mismatch");
            return false;
        }

        // Validates the image (header, chip, checksums, the appended hash).
        if (esp_ota_end(handle) != ESP_OK)
        {
            sendFrame(fd, 'T', "{\"state\":\"failed\",\"pct\":100,\"msg\":\"not a valid image for this chip\"}");
            fail("not a valid image for this chip");
            return false;
        }
        if (esp_ota_set_boot_partition(slot) != ESP_OK)
        {
            sendFrame(fd, 'T', "{\"state\":\"failed\",\"pct\":100,\"msg\":\"could not select the new image\"}");
            fail("could not select the new image");
            return false;
        }

        setPercent(100);
        gReason.store(nullptr);
        setState(OtaState::Done);
        sendStatus(fd, "done", 100);
        puts("[ota] image written; restarting");
        return true;
    }

    /// One PC, from accept to close.
    bool session(int fd)
    {
        timeval receiveWait = {1, 0};
        timeval sendWait = {10, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receiveWait, sizeof(receiveWait));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendWait, sizeof(sendWait));
        int noDelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

        // Challenge.
        uint8_t raw[16];
        esp_fill_random(raw, sizeof(raw));
        char nonce[33];
        toHex(raw, sizeof(raw), nonce);
        // The whole 32-byte hash, not esp_app_get_elf_sha256(): that one returns
        // only as many characters as CONFIG_APP_RETRIEVE_LEN_ELF_SHA keeps (9
        // bytes by default), and the PC compares all 64.
        char elf[65] = "";
        toHex(esp_app_get_description()->app_elf_sha256, 32, elf);
        char challenge[256];
        snprintf(challenge, sizeof(challenge),
                 "{\"v\":0,\"nonce\":\"%s\",\"device\":\"%s\",\"fw\":\"%s\",\"elf\":\"%s\"}",
                 nonce, gDeviceIdentity.getDeviceName().c_str(), VERSION, elf);
        if (!sendFrame(fd, 'C', challenge))
        {
            return false;
        }

        Frame frame;
        char hello[JsonCap];
        if (!readHeader(fd, frame, HelloWaitMs) || frame.kind != 'H' ||
            !readJson(fd, frame, hello, sizeof(hello)))
        {
            return false;
        }
        char name[64] = "";
        char proof[96] = "";
        char want[65];
        const bool authentic = jsonString(hello, "name", name, sizeof(name)) &&
                               jsonString(hello, "auth", proof, sizeof(proof)) &&
                               proofFor(nonce, name, want) && sameText(proof, want);
        if (!authentic)
        {
            sendFrame(fd, 'E', "{\"code\":\"auth\",\"msg\":\"pairing token does not match\"}");
            puts("[ota] refused a PC: bad token");
            sleepMs(1000); // not a free oracle for guessing
            return false;
        }
        char welcome[40];
        snprintf(welcome, sizeof(welcome), "{\"heartbeat_s\":%lu}", (unsigned long)HeartbeatSeconds);
        if (!sendFrame(fd, 'W', welcome))
        {
            return false;
        }
        printf("[ota] linked to %s\n", name);
        gReason.store(nullptr);
        setState(OtaState::Linked);

        while (readHeader(fd, frame, SilenceLimitMs))
        {
            if (frame.kind == 'P')
            {
                answerPing(fd, frame);
            }
            else if (frame.kind == 'U')
            {
                if (receiveImage(fd, frame))
                {
                    return true;
                }
                return false;
            }
            else if (!discard(fd, frame))
            {
                break; // state, results, data nobody asked for: read and dropped
            }
        }
        return false;
    }

    void otaTask(void *)
    {
        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        bool restart = false;
        if (listener >= 0)
        {
            int reuse = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_ANY);
            address.sin_port = htons(LinkPort);
            if (bind(listener, (sockaddr *)&address, sizeof(address)) == 0 && listen(listener, 1) == 0)
            {
                printf("[ota] update mode open on port %u\n", (unsigned)LinkPort);
                while (!gStop.load() && (int32_t)(millisNow() - gCloseAtMs.load()) < 0)
                {
                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(listener, &readable);
                    timeval wait = {1, 0};
                    if (select(listener + 1, &readable, nullptr, nullptr, &wait) <= 0)
                    {
                        continue;
                    }
                    const int client = accept(listener, nullptr, nullptr);
                    if (client < 0)
                    {
                        continue;
                    }
                    restart = session(client);
                    shutdown(client, SHUT_RDWR);
                    close(client);
                    if (restart)
                    {
                        break;
                    }
                    if (gState.load() != (uint8_t)OtaState::Failed)
                    {
                        setState(OtaState::Listening);
                    }
                    gCloseAtMs.store(millisNow() + IdleCloseMs);
                }
            }
            else
            {
                fail("port 7340 is not available");
            }
            close(listener);
        }
        else
        {
            fail("no socket");
        }

        if (restart)
        {
            sleepMs(1000); // the "done" frame is out; give the PC a moment to read it
            esp_restart();
        }
        puts("[ota] update mode closed");
        setState(OtaState::Off);
        vTaskDelete(nullptr);
    }
}

void Ota_begin()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t imageState;
    pendingVerify = running && esp_ota_get_state_partition(running, &imageState) == ESP_OK &&
                    imageState == ESP_OTA_IMG_PENDING_VERIFY;
    if (pendingVerify)
    {
        puts("[ota] running a new image on probation; it is marked good if it survives 20 s");
    }
}

bool Ota_start(std::string *why)
{
    const auto refuse = [&](const char *reason)
    {
        if (why)
        {
            *why = reason;
        }
        return false;
    };

    if (gState.load() != (uint8_t)OtaState::Off)
    {
        if (gStop.load())
        {
            return refuse("still closing, try again in a moment");
        }
        return true; // already open
    }
    if (strlen(DASHLINK_TOKEN) == 0)
    {
        return refuse("no DASHLINK_TOKEN in include/creds.h");
    }
    if (WiFi_info().state != NightMare::WiFiState::CONNECTED)
    {
        return refuse("Wi-Fi is not connected");
    }

    gStop.store(false);
    gReason.store(nullptr);
    setPercent(0);
    gCloseAtMs.store(millisNow() + IdleCloseMs);
    setState(OtaState::Listening);
    // Core 0, with the Wi-Fi and lwIP tasks; the main loop is on core 1.
    if (xTaskCreatePinnedToCore(otaTask, "ota", 8192, nullptr, 3, nullptr, 0) != pdPASS)
    {
        setState(OtaState::Off);
        return refuse("not enough memory to start update mode");
    }
    return true;
}

void Ota_stop()
{
    gStop.store(true);
}

bool Ota_active()
{
    return gState.load() != (uint8_t)OtaState::Off;
}

OtaState Ota_state()
{
    return (OtaState)gState.load();
}

uint8_t Ota_percent()
{
    return gPercent.load();
}

uint32_t Ota_revision()
{
    return gRevision.load();
}

std::string Ota_message()
{
    const char *reason = gReason.load();
    switch (Ota_state())
    {
    case OtaState::Off:
        return reason ? std::string("last attempt failed: ") + reason : std::string("");
    case OtaState::Listening:
    {
        const NightMare::WiFiInfo wifi = WiFi_info();
        std::string text = "waiting for the PC at " + wifi.ip + ":" + std::to_string(LinkPort);
        if (reason)
        {
            text += std::string("\nlast attempt failed: ") + reason;
        }
        return text;
    }
    case OtaState::Linked:
        return "PC linked, waiting for the image";
    case OtaState::Receiving:
        return "receiving " + std::to_string(Ota_percent()) + "%";
    case OtaState::Verifying:
        return "checking the image";
    case OtaState::Done:
        return "written, restarting";
    case OtaState::Failed:
        return std::string("failed: ") + (reason ? reason : "unknown");
    }
    return "";
}

void Ota_tick()
{
    static uint32_t lastCheckMs = 0;
    const uint32_t now = millisNow();
    if ((now - lastCheckMs) < 500)
    {
        return;
    }
    lastCheckMs = now;

    // The image has been running long enough, on a loop that is clearly
    // alive: this is what the bootloader's rollback is waiting for.
    if (!marked && now >= MarkValidAfterMs)
    {
        marked = true;
        if (pendingVerify)
        {
            esp_ota_mark_app_valid_cancel_rollback();
            puts("[ota] new image marked valid");
        }
    }

    // A freshly updated panel offers the PC the chance to check which build
    // it is running. It cannot do that until the station has an address, so
    // this keeps trying through the first minute.
    if (pendingVerify && !Ota_active() && now < ProbationListenMs)
    {
        if (Ota_start(nullptr))
        {
            gCloseAtMs.store(ProbationListenMs);
        }
    }
}
