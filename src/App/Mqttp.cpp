#include "Mqttp.h"
#include <NightMare.h>

namespace
{
    // One block of MQTTP_RESPONSE_MAX, allocated when a request is made and freed
    // by Mqttp_cancel() -- which the device page calls when it is left. Never
    // grown, so it cannot fragment the heap the way a String reassembling chunks
    // would (see Mqttp.h). Not static storage: that held 3KB permanently for a
    // page that is almost never open, out of the same heap the TLS handshake
    // needs.
    char *responseBuffer = nullptr;
    size_t responseLength = 0;
    bool truncated = false;

    MqttpState state = MQTTP_IDLE;
    uint32_t revision = 1;
    uint32_t deadline = 0;

    String currentDevice;
    String currentCommand;
    /// The device whose reply space is currently subscribed, and the filter it
    /// was subscribed under.
    String listeningDevice;
    String listeningFilter;
    /// The reply topic of the request in flight, lowercased for matching. The
    /// firmware derives the reply topic from the request topic, so the casing
    /// does come back as it was sent -- the lowercase compare is only belt and
    /// braces against a device that rebuilds it from its own name.
    String expectedTopicLower;

    void resetBuffer()
    {
        responseLength = 0;
        truncated = false;
        if (responseBuffer)
        {
            responseBuffer[0] = '\0';
        }
    }

    /// Append what fits, and remember if anything did not.
    void appendChunk(const String &data)
    {
        if (!responseBuffer)
        {
            return;
        }
        size_t room = MQTTP_RESPONSE_MAX - responseLength;
        size_t take = data.length();
        if (take > room)
        {
            take = room;
            truncated = true;
        }
        if (take)
        {
            memcpy(responseBuffer + responseLength, data.c_str(), take);
            responseLength += take;
            responseBuffer[responseLength] = '\0';
        }
    }

    void finish(MqttpState result)
    {
        state = result;
        expectedTopicLower = "";
        revision++;
    }
}

bool Mqttp_listenTo(const String &device)
{
    if (listeningDevice == device)
    {
        return listeningFilter.length() != 0;
    }
    Mqttp_stopListening();
    if (!device.length())
    {
        return false;
    }

    // One level of wildcard, covering every id this panel might use on that
    // device -- and only that device, so the panel does not carry the rest of
    // the network's command traffic through its inbox.
    const String filter = device + "/console/controlled/+/out";
    if (!MQTT_SubscribeTopic(filter))
    {
        return false;
    }
    listeningDevice = device;
    listeningFilter = filter;
    return true;
}

void Mqttp_stopListening()
{
    Mqttp_cancel();
    if (listeningFilter.length())
    {
        MQTT_UnsubscribeTopic(listeningFilter);
        listeningFilter = "";
    }
    listeningDevice = "";
}

bool Mqttp_request(const String &device, const String &command)
{
    if (!device.length() || !command.length() || !MQTT_Connected())
    {
        return false;
    }

    // Allocated here, where a failure can simply be reported, rather than kept
    // around permanently. A plain malloc checks its result -- unlike an LVGL
    // widget allocation, which crashes when the heap runs out.
    if (!responseBuffer)
    {
        responseBuffer = (char *)malloc(MQTTP_RESPONSE_MAX + 1);
        if (!responseBuffer)
        {
            return false;
        }
    }

    // Normally the page has been listening since it opened, which is the whole
    // point -- a subscription installed here would still be in flight when the
    // answer came back. Doing it anyway keeps a caller that forgot from failing
    // silently; it just races on that first request.
    if (!Mqttp_listenTo(device))
    {
        return false;
    }

    // A fresh id per request, so a late reply to an abandoned one is ignored
    // rather than mistaken for the answer to this. The protocol leaves
    // uniqueness entirely to the caller: the device keeps no registry of ids and
    // simply answers on the topic it was asked on.
    char id[9];
    snprintf(id, sizeof(id), "%08x", (unsigned)esp_random());

    currentDevice = device;
    currentCommand = command;

    const String base = device + "/console/controlled/" + id;
    expectedTopicLower = base + "/out";
    expectedTopicLower.toLowerCase();

    resetBuffer();
    state = MQTTP_PENDING;
    deadline = millis() + MQTTP_TIMEOUT_MS;
    revision++;

    MQTT_Send(base + "/in", command, false, false);
    return true;
}

void Mqttp_cancel()
{
    resetBuffer();
    free(responseBuffer);
    responseBuffer = nullptr;
    state = MQTTP_IDLE;
    expectedTopicLower = "";
    revision++;
}

void Mqttp_tick()
{
    if (state != MQTTP_PENDING)
    {
        return;
    }
    // Unsigned comparison, so this survives the millis() rollover.
    if ((int32_t)(millis() - deadline) >= 0)
    {
        finish(MQTTP_TIMEOUT);
    }
}

bool Mqttp_handleMessage(const String &topic, const String &payload)
{
    if (state != MQTTP_PENDING || !expectedTopicLower.length())
    {
        return false;
    }

    String lowered = topic;
    lowered.toLowerCase();
    if (lowered != expectedTopicLower)
    {
        return false;
    }

    // Unchunked: the whole reply in one message. Anything at or below the
    // sender's 512-byte chunk size arrives this way.
    if (!payload.startsWith(";;"))
    {
        appendChunk(payload);
        finish(MQTTP_DONE);
        return true;
    }

    const int headerEnd = payload.indexOf(";;", 2);
    if (headerEnd < 0)
    {
        // Starts like a chunk header but is not one; take it at face value.
        appendChunk(payload);
        finish(MQTTP_DONE);
        return true;
    }

    const String header = payload.substring(2, headerEnd);
    // The data may itself contain ";;", so everything past the header belongs
    // to the payload -- do not split on further delimiters.
    const String data = payload.substring(headerEnd + 2);

    if (header == "error")
    {
        appendChunk(data);
        finish(MQTTP_FAILED);
        return true;
    }

    const int slash = header.indexOf('/');
    if (slash < 0)
    {
        appendChunk(data);
        finish(MQTTP_DONE);
        return true;
    }

    const long current = header.substring(0, slash).toInt();
    const long total = header.substring(slash + 1).toInt();

    appendChunk(data);
    revision++;

    if (current >= total)
    {
        finish(MQTTP_DONE);
    }
    else
    {
        // Each chunk proves the device is still answering, so give it room to
        // finish rather than timing out mid-transfer on a long reply.
        deadline = millis() + MQTTP_TIMEOUT_MS;
    }
    return true;
}

MqttpState Mqttp_state()
{
    return state;
}

const char *Mqttp_response()
{
    return responseBuffer ? responseBuffer : "";
}

bool Mqttp_truncated()
{
    return truncated;
}

const String &Mqttp_command()
{
    return currentCommand;
}

const String &Mqttp_device()
{
    return currentDevice;
}

uint32_t Mqttp_revision()
{
    return revision;
}
