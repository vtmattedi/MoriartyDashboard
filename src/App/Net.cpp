#include "Net.h"
#include "Targets.h"
#include "Registry.h"
#include "Forecast.h"
#include "Mqttp.h"
#include "AcClient.h"
#include "LightControl.h"
#include "Heap.h"

#include <NightMare.h>
#include <Network/EspNow/EspNowClient.h>
#include <Network/NmConnectionInternal.h>
#include <Network/NmMessageRouter.h>
#include <freertos/ringbuf.h>
#include <atomic>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Threading
//
// ESP-NOW delivers its message callback from the Wi-Fi event task, which can
// run truly in parallel with loop() on this dual-core chip. Everything a
// message touches here (the registry's Strings, the MQTTP buffer, the forecast)
// is also read and written by loop() for the UI. Doing that work on both tasks
// at once corrupts the heap.
//
// So the callback does no application work at all. It writes the frame into a
// ring buffer and returns without blocking. loop() first offers it to the
// framework router, then routes anything unconsumed to the dashboard. Every
// piece of application state is therefore touched by one task. No mutexes.
//
// A no-split ring buffer rather than a queue of fixed-size slots: messages vary
// from ~40 bytes (most resource states) to ~530 (an MQTTP reply chunk), and
// fixed slots sized for the largest waste most of their memory. Here each
// message costs only its own length, storage is allocated once at Net_begin(),
// and the event task writes straight into it.
//
// If loop() falls behind and the buffer fills, new messages are dropped (and
// counted) rather than blocking the Wi-Fi event task.
// ---------------------------------------------------------------------------

namespace
{
    bool publishText(const String &topic, const String &payload, bool retained = false)
    {
        return NightMare::Publish(topic.c_str(),
                                  reinterpret_cast<const uint8_t *>(payload.c_str()),
                                  payload.length(), retained);
    }

    /// Long enough for every topic the panel routes. The longest is an MQTTP
    /// reply, "<device>/console/controlled/<id>/out", and both segments may run
    /// to 64 characters.
    constexpr size_t TopicMax = 160;
    /// Comfortably above the largest payload the panel parses: an MQTTP reply
    /// chunk is at most 512 bytes plus its ";;n/total;;" header, and a resource
    /// value is capped by the protocol at 2048 but is in practice a number.
    constexpr size_t PayloadMax = 2304;
    /// Total inbox storage. Holds ~100 typical resource states, or a burst of
    /// several full MQTTP chunks at once.
    constexpr size_t InboxBytes = 12 * 1024;
    /// Per loop() pass, so a flood cannot starve LVGL of time to draw. A
    /// reconnect can replay retained state through the gateway at once, which
    /// is the burst this has to survive without dropping frames.
    constexpr size_t DrainPerLoop = 8;

    /// How often the bound resources are copied into the registry. Once a
    /// second: it is only there so they stay listed and unbindable, and the
    /// scan is a string compare per registry entry.
    constexpr uint32_t MirrorIntervalMs = 1000;

    // The panel's own subscriptions. The library takes only what its own
    // features and bound resources need, so everything the Devices and
    // Resources screens show is asked for here. Custom subscriptions are
    // remembered in RAM and reinstalled on every reconnect, so this is done once.
    const char *const Subscriptions[] = {
        "+/status",
        "+/resource/+/state",
        "Control/forecast",
    };

    RingbufHandle_t inbox = nullptr;
    std::atomic<uint32_t> droppedFull{0};
    std::atomic<uint32_t> droppedOversize{0};
    bool transportHooked = false;
    bool wasConnected = false;

    uint32_t appliedTargetsRevision = 0;
    uint32_t lastMirrorMs = 0;

    /// When to take the second reading after a connection comes up. 0 when none
    /// is pending. See the connect branch of Net_loop() for what the pair is for.
    uint32_t burstSampleAtMs = 0;
    /// Long enough for the gateway to replay retained status and resource
    /// state, but short enough to remain inside the connection spike.
    constexpr uint32_t BurstSampleDelayMs = 3000;

    // Startup heap trace -- temporary scaffolding, see Net_traceStartup().
    uint32_t traceStartedMs = 0;
    uint32_t traceNextSampleMs = 0;
    bool tracingStartup = false;
    constexpr uint32_t TraceIntervalMs = 500;
    /// Gives up on its own, so a panel that never reaches the gateway does not
    /// log a line every half second until someone notices.
    constexpr uint32_t TraceMaxMs = 90000;

    // Heap reporting lives in App/Heap.h, which knows which pool is the real
    // one. Net_droppedMessages() is folded into that line because the two are
    // read together: a rising drop count next to a falling heap is loop() being
    // starved, and next to a healthy one it is something else.

    /// Wi-Fi event task. Write into the inbox; nothing else.
    void onEspNowMessage(const char *topic, const uint8_t *payload,
                         size_t payloadLength, bool)
    {
        if (!inbox || !topic || (!payload && payloadLength))
        {
            return;
        }

        const size_t topicLength = strlen(topic);

        // Counted only once it is known to be something the panel wanted, so
        // the drop counter means "lost", not "ignored".
        if (topicLength > TopicMax || payloadLength > PayloadMax)
        {
            droppedOversize++;
            return;
        }

        // topic, NUL, payload, NUL
        const size_t itemSize = topicLength + 1 + payloadLength + 1;

        // Acquire space and write straight into it: no staging copy. Never wait
        // -- see the note at the top of the file.
        void *slot = nullptr;
        if (xRingbufferSendAcquire(inbox, &slot, itemSize, 0) != pdTRUE || !slot)
        {
            droppedFull++;
            return;
        }

        char *cursor = (char *)slot;
        memcpy(cursor, topic, topicLength + 1);
        cursor += topicLength + 1;
        if (payloadLength)
        {
            memcpy(cursor, payload, payloadLength);
        }
        cursor[payloadLength] = '\0';

        xRingbufferSendComplete(inbox, slot);
    }

#pragma region "Routing (loop task only)"

    /// Topic prefixes that are channels rather than devices, so they never end
    /// up in the device registry. Mirrors the backend's own ignore list.
    bool isReservedName(const String &name)
    {
        String lowered = name;
        lowered.toLowerCase();
        return lowered == "control" || lowered == "n8n" || lowered == "all" ||
               lowered.startsWith("$");
    }

    void route(const char *topicText, const char *payloadText)
    {
        const String topic(topicText);
        const String payload(payloadText);

        // The low-level ESP-NOW callback is temporarily the only application
        // ingress hook in NightMareNetwork. Preserve normal framework routing
        // for resources, console commands and time before handling dashboard
        // wildcard traffic.
        if (NmMessageRouter::handleMessage(topic, payload))
        {
            return;
        }

        // Control/time is not handled here: the library consumes it inside the
        // MQTT client and sets the clock itself, before this callback runs.
        if (topic == "Control/forecast")
        {
            Forecast_handle(topic, payload);
            return;
        }

        // MQTTP replies ride in on the panel's own per-request subscription.
        if (Mqttp_handleMessage(topic, payload))
        {
            return;
        }

        const int firstSlash = topic.indexOf('/');
        const String device = topic.substring(0, firstSlash);
        if (isReservedName(device))
        {
            return;
        }

        const String rest = topic.substring(firstSlash + 1);

        if (rest == "status")
        {
            // An empty payload here is a retained clear, which Registry_noteStatus
            // reads as "offline" without creating an entry -- the panel's own
            // clears in Net_deleteDevice() come straight back on "+/status".
            Registry_noteStatus(device, payload);
            return;
        }

        // "resource/<name>/state", shape already checked on the MQTT task.
        if (rest.startsWith("resource/"))
        {
            // An empty payload withdraws the retained state. The last known
            // value stays readable, which is what the protocol says a tombstone
            // means -- and not resurrecting anything is what lets
            // Net_deleteDevice()'s own clears echo back harmlessly.
            if (!payload.length())
            {
                return;
            }
            const int nameStart = sizeof("resource/") - 1;
            const int nameEnd = rest.lastIndexOf('/');
            if (nameEnd > nameStart)
            {
                Registry_noteResource(device, rest.substring(nameStart, nameEnd), payload);
            }
        }
    }

#pragma endregion

    void forecastJob()
    {
        Forecast_request();
    }

    void heapJob()
    {
        Heap_log("periodic");
        // Beside the heap line on purpose: the two answer the same question
        // from opposite ends. If the screens are short, either the table is
        // evicting or the rows could not be allocated, and one line each says
        // which.
        Registry_logSummary();
    }
}

void Net_controlRequest(const char *what)
{
    if (NightMare::GetConnectionState() != NightMare::ConnectionState::CONNECTED)
    {
        return;
    }
    // Raw: the Control channel is shared, not owned by this device, so the
    // usual "<device>/" prefix must not be prepended.
    publishText("Control/request", what);
}

bool Net_requestAllManifests()
{
    if (NightMare::GetConnectionState() != NightMare::ConnectionState::CONNECTED)
    {
        return false;
    }
    // Broadcast console commands are addressed to the reserved `all` target.
    // Do not prepend this panel's device name to the topic.
    return publishText("all/console/in", ">manifest msgpack");
}

bool Net_rebootDevice(const String &deviceName)
{
    if (!deviceName.length() ||
        NightMare::GetConnectionState() != NightMare::ConnectionState::CONNECTED)
    {
        return false;
    }
    // insertOwner = false: the command is addressed to that device's console,
    // not to this panel's own. REBOOT is a console built-in, so every NightMare
    // device answers it without a project resolver.
    return publishText(deviceName + "/console/in", "REBOOT");
}

bool Net_deleteDevice(const String &deviceName)
{
    if (!deviceName.length() ||
        NightMare::GetConnectionState() != NightMare::ConnectionState::CONNECTED)
    {
        return false;
    }

    // Snapshot the resource names before forgetting the device: they are what
    // the retained state topics are built from, and after Registry_forgetDevice
    // they are gone.
    String resourceNames[REGISTRY_MAX_RESOURCES];
    int resourceNameCount = 0;
    for (int i = 0; i < Registry_resourceCount(); ++i)
    {
        const ResourceEntry *entry = Registry_resource(i);
        if (entry && entry->device == deviceName)
        {
            resourceNames[resourceNameCount++] = entry->resource;
        }
    }

    // Leave memory first, then clear the broker. The clears come straight back
    // on the wildcard subscriptions, and the tombstone rules in route() and
    // Registry_noteStatus only hold because the entry is already gone by the
    // time they arrive.
    // Every slot, not just the first: roles are not exclusive by device, so one
    // board can be the AC and own the bound colour as well.
    for (int slot = 0; slot < TARGET_SLOT_COUNT; ++slot)
    {
        if (Targets_device((TargetSlot)slot) == deviceName)
        {
            Targets_set((TargetSlot)slot, "", "");
        }
    }
    Registry_forgetDevice(deviceName);

    // An empty retained payload is the protocol's deletion marker. The manifest
    // and the status both have to go: either one left retained re-announces the
    // device the next time anything resubscribes.
    bool published = publishText(deviceName + "/status", "", true);
    published = publishText(deviceName + "/manifest", "", true) && published;
    published = publishText(deviceName + "/manifest/msgpack", "", true) && published;
    for (int i = 0; i < resourceNameCount; ++i)
    {
        published = publishText(deviceName + "/resource/" + resourceNames[i] + "/state",
                                "", true) &&
                    published;
    }
    return published;
}

uint32_t Net_droppedMessages()
{
    return droppedFull.load() + droppedOversize.load();
}

void Net_begin()
{
    inbox = xRingbufferCreate(InboxBytes, RINGBUF_TYPE_NOSPLIT);
    if (!inbox)
    {
        Serial.println("[net] inbox allocation failed; network messages will be ignored");
    }

    // Accepted and remembered before ESP-NOW is up, then restored when the
    // gateway connection is established.
    for (const char *filter : Subscriptions)
    {
        if (!NightMare::Subscribe(filter))
        {
            Serial.printf("[net] could not subscribe to %s\n", filter);
        }
    }

    // The backend only answers what it is asked, and a panel that missed a
    // reply should not stay wrong until the next reboot. Time sync needs no job
    // of its own any more: the library runs SNTP and asks on Control/request
    // whenever its clock is not valid.
    //
    // These are callbacks, so they are MANAGED jobs -- JOB CLEAR on the console
    // cannot remove them -- and with NM_SCHEDULER_OWN_TASK 0 they run on the
    // loop() task, which is what lets them touch the forecast at all.
    gScheduler.everyMonotonic("dash.forecast", forecastJob, 900000);
    gScheduler.everyMonotonic("dash.heap", heapJob, 30000);
}

void Net_traceStartup()
{
    traceStartedMs = millis();
    traceNextSampleMs = traceStartedMs;
    tracingStartup = true;
}

void Net_loop()
{
    // NmEspNowConnection owns the only low-level callback. Install the
    // dashboard bridge after the framework starts; state callbacks remain with
    // the framework, while message routing moves onto loop().
    if (!transportHooked &&
        NightMare::GetSelectedConnection() == NightMare::ConnectionType::ESP_NOW &&
        NightMare::GetConnectionState() != NightMare::ConnectionState::STOPPED)
    {
        NightMare::EspNowClient::onMessage(onEspNowMessage);
        transportHooked = true;
    }

    const bool connected =
        NightMare::GetConnectionState() == NightMare::ConnectionState::CONNECTED;
    // Draw the startup curve across radio start, Wi-Fi association, gateway
    // discovery and the retained replay.
    if (tracingStartup)
    {
        const uint32_t elapsed = millis() - traceStartedMs;
        if (elapsed > TraceMaxMs)
        {
            tracingStartup = false;
        }
        else if ((int32_t)(millis() - traceNextSampleMs) >= 0)
        {
            traceNextSampleMs += TraceIntervalMs;
            char stage[16];
            snprintf(stage, sizeof(stage), "t+%lu.%lus", (unsigned long)(elapsed / 1000),
                     (unsigned long)((elapsed % 1000) / 100));
            Heap_log(stage);
        }
    }

    // Network console commands are routed on loop(), but staging also keeps the
    // serial and UI paths identical.
    Targets_applyPending();

    // A binding changed: re-point the remote resources at whatever now fills
    // each role. Done here rather than inside Targets_set so it always happens
    // on the loop() task, whichever screen or command asked for it.
    const uint32_t targetsRevision = Targets_revision();
    if (targetsRevision != appliedTargetsRevision)
    {
        appliedTargetsRevision = targetsRevision;
        AcClient_applyTargets();
        LightControl_applyTargets();
    }

    if (connected && !wasConnected)
    {
        // Bracket gateway discovery and the first subscription replay; the
        // periodic 30-second heap sample is too coarse to show this burst.
        tracingStartup = false; // The curve ends here; the burst pair takes over.
        Heap_log("espnow up");
        burstSampleAtMs = millis() + BurstSampleDelayMs;

        // The current upstream startup path gates its automatic status publish
        // on the MQTT feature. ESP-NOW still uses the same retained status
        // contract, so announce Moriarty explicitly until that gate moves to
        // NM_ENABLE_NETWORK upstream.
        publishText(gDeviceIdentity.topic("status"),
                    NightMare::ConnectionDeviceStatusJson(true), true);

        // Ask for the one thing only the backend can answer, as soon as there is
        // someone to answer it. Every resource state and device status is
        // retained, so the gateway replays those unprompted.
        Forecast_request();
    }
    wasConnected = connected;

    // Unsigned comparison, so this survives the millis() rollover.
    if (burstSampleAtMs && (int32_t)(millis() - burstSampleAtMs) >= 0)
    {
        burstSampleAtMs = 0;
        Heap_log("after burst");
    }

    for (size_t i = 0; inbox && i < DrainPerLoop; ++i)
    {
        size_t size = 0;
        char *item = (char *)xRingbufferReceive(inbox, &size, 0);
        if (!item)
        {
            break;
        }

        // Validate the layout before trusting it: two NUL-terminated strings,
        // both within the item's own size. Routed in place -- the item's memory
        // is valid until it is returned below.
        const size_t topicLength = strnlen(item, size);
        if (size >= 2 && topicLength < size)
        {
            const char *payload = item + topicLength + 1;
            const size_t payloadRoom = size - topicLength - 1;
            if (payloadRoom > 0 && strnlen(payload, payloadRoom) < payloadRoom)
            {
                route(item, payload);
            }
        }

        vRingbufferReturnItem(inbox, item);
    }

    Mqttp_tick();

    const uint32_t nowMs = millis();
    if ((nowMs - lastMirrorMs) >= MirrorIntervalMs)
    {
        lastMirrorMs = nowMs;
        LightControl_mirrorToRegistry();
    }
}
