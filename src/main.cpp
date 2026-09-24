#include <Arduino.h>
#include <boardstuff.h>
#include <NightMare.h>

#include "App/Net.h"
#include "App/Heap.h"
#include "App/Registry.h"
#include "App/Targets.h"
#include "App/Forecast.h"
#include "App/SensorBindings.h"
#include "App/AcClient.h"
#include "App/LightControl.h"
#include "UI/Ui.h"

// NightMare Network wall dashboard.
//
// A panel with no hardware of its own: it joins the network as an ordinary
// NightMare device, watches every device and resource on the broker, and drives
// an AC, a light and a colour light that live on other devices. See App/Net.h
// for how messages are routed and App/Targets.h for how the bindings are stored.

#pragma region "Console"

/// Console command: report or change what fills each role.
///   TARGET                          the current bindings, as JSON
///   TARGET AC Adler                 bind the AC role to "Adler"
///   TARGET LIGHT Mycroft light      bind the light role to Mycroft's "light"
///   TARGET RGB Sherlock             bind the colour role, default resource name
///   TARGET LIGHT ""                 unbind
///
/// Device and resource names keep their original casing on purpose: MQTT topic
/// segments are case-sensitive, so "Micro" and "micro" are two different
/// devices.
static NightMareResults handleTargetCommand(const NightMareMessage &message)
{
    NightMareResults res;

    if (!message.subcommand.length())
    {
        // The snapshot rather than the bindings themselves: a command that came
        // in over MQTT is running on the MQTT task, and copying a String that
        // loop() may be reallocating is how that goes wrong.
        char summary[256];
        Targets_summaryJson(summary, sizeof(summary));
        res.response = summary;
        res.result = true;
        return res;
    }

    int slot = -1;
    for (int i = 0; i < TARGET_SLOT_COUNT; ++i)
    {
        if (message.subcommand.equalsIgnoreCase(Targets_label((TargetSlot)i)))
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        res.result = false;
        res.response = "Unknown role. Expected one of: AC, LIGHT, RGB.";
        return res;
    }

    // Staged rather than applied: a command that arrived over MQTT is running
    // on the MQTT client's task, and the bindings belong to loop(). Net_loop()
    // picks it up within a few milliseconds.
    //
    // args[0] is the subcommand, so the device is args[1] and the resource
    // args[2]. An omitted resource keeps whatever the slot already had, which
    // for a fresh slot is the role's default name.
    const bool haveResource = message.argc >= 3 && message.args[2].length();
    if (!Targets_request((TargetSlot)slot, message.args[1],
                         haveResource ? message.args[2] : String(""), !haveResource))
    {
        res.result = false;
        res.response = "Another binding change is still being applied. Try again.";
        return res;
    }
    Ui_markDirty();

    // What was asked for, not what is bound: the change has not been applied
    // yet, and reading the bindings from this task is exactly what staging the
    // request avoids.
    res.result = true;
    res.response = String("{\"device\":\"") + message.args[1] + "\",\"resource\":\"" +
                   (haveResource ? message.args[2] : String(Targets_defaultResource((TargetSlot)slot))) +
                   "\"}";
    return res;
}

static NightMareResults localHandleNightMareCommand(const NightMareMessage &message)
{
    if (message.command == "TARGET")
    {
        return handleTargetCommand(message);
    }

    if (message.command == "FORECAST")
    {
        Forecast_request();
        NightMareResults res;
        res.result = true;
        res.response = "Forecast requested.";
        return res;
    }

    if (message.command == "HEAP")
    {
        NightMareResults res;
        res.result = true;
        // The 8-bit heap, which is the one anything can allocate from. Largest
        // free block matters as much as the total: mbedTLS needs its handshake
        // buffers contiguous, and a fragmented heap fails even when the total
        // looks comfortable. `internalFree` is what ESP.getFreeHeap() reports,
        // IRAM included; it is here to be compared against, not acted on.
        res.response = String("{\"free\":") + Heap_free() +
                       ",\"minFree\":" + Heap_minFree() +
                       ",\"largestBlock\":" + Heap_largest() +
                       ",\"internalFree\":" + Heap_freeInternal() +
                       ",\"droppedMessages\":" + Net_droppedMessages() + "}";
        return res;
    }

    if (message.command == "REGISTRY")
    {
        // Prints rather than returns: the full table is far past what fits in
        // an MQTTP reply, and the question it answers -- did the panel hear
        // this resource at all -- is one you ask with the console open.
        Registry_logAll();
        NightMareResults res;
        res.result = true;
        res.response = String("{\"devices\":") + Registry_deviceCount() +
                       ",\"resources\":" + Registry_resourceCount() +
                       ",\"evictedDevices\":" + Registry_evictedDevices() +
                       ",\"evictedResources\":" + Registry_evictedResources() +
                       ",\"maxDevices\":" + REGISTRY_MAX_DEVICES +
                       ",\"maxResources\":" + REGISTRY_MAX_RESOURCES + "}";
        return res;
    }

    NightMareResults res;
    res.result = false;
    res.response = "Unknown command. Available: [TARGET, FORECAST, HEAP, REGISTRY]. "
                   "Framework built-ins and `>` resource commands still apply.";
    return res;
}

#pragma endregion

static bool networkStarted = false;

/// Joining the network needs a lot of heap at once, and the expensive part is
/// out of the panel's hands: the library brings MQTT up from inside the WiFi
/// task on the first connection, and the IDF client duplicates its config (root
/// CA included) before mbedTLS asks for tens of KB *contiguous* for the
/// handshake. Short of that, esp_mqtt_client_init() does not fail cleanly -- it
/// hands back a half-built handle and the first publish dereferences a null,
/// panicking the core and boot-looping the panel.
///
/// So the check moved to the last point the panel still controls: WiFi is not
/// started at all until there is room for what follows it. That is a weaker
/// guarantee than checking immediately before the handshake -- association
/// itself costs heap in between -- but it is the difference between a panel
/// sitting on the wall with a red MQTT icon, which is debuggable, and a reboot
/// loop, which is not.
static void tryStartNetwork()
{
    if (networkStarted)
    {
        return;
    }

    // Both tests, and both against the 8-bit heap: the handshake needs roughly
    // 50-60KB in total and two 16KB record buffers *contiguous*, so a
    // fragmented heap fails it while the total still looks comfortable. This
    // used to read ESP.getFreeHeap(), which counts IRAM the handshake cannot
    // touch -- see App/Heap.h.
    const size_t freeHeap = Heap_free();
    const size_t largestBlock = Heap_largest();
    const size_t requiredHeap = 60 * 1024;
    const size_t requiredBlock = 20 * 1024;
    if (freeHeap < requiredHeap || largestBlock < requiredBlock)
    {
        static uint32_t lastComplaintMs = 0;
        if (millis() - lastComplaintMs > 5000)
        {
            lastComplaintMs = millis();
            Serial.printf("[net] deferred: %u free / %u largest, need %u / %u\n",
                          (unsigned)freeHeap, (unsigned)largestBlock,
                          (unsigned)requiredHeap, (unsigned)requiredBlock);
        }
        return;
    }

    Serial.printf("[net] starting with %u free / %u largest\n",
                  (unsigned)freeHeap, (unsigned)largestBlock);
    networkStarted = true;
    // Temporary: trace the heap across association, MQTT init and the
    // handshake. Started before, not after, or the expensive part is over
    // before the first sample.
    Net_traceStartup();
    // Scheduler, telemetry and WiFi_Auto(); the first WiFi connection then
    // starts MQTT and SNTP.
    startNightMareESP();
}

void setup()
{
    // Serial, the timezone, the settings store, this device's identity and the
    // banner. Nothing is started: that is startNightMareESP(), below.
    introNightMareESP();

    // Display first: a panel that shows nothing while it joins the network
    // looks broken, and WiFi association can take a few seconds.
    Heap_log("boot");
    board_init();
    lvgl_begin();
    Heap_log("after lvgl");

    Targets_load();
    SensorBindings_load();
    Ui_begin();
    Heap_log("after ui");

    setCommandResolver(localHandleNightMareCommand);

    // Declared before the framework starts, for two reasons: a pending identity
    // cleanup can only withdraw resources that are already declared, and the
    // resource manager installs each subscription as soon as it knows the
    // address, so binding first means nothing is missed on the first connect.
    AcClient_begin();
    LightControl_begin();
    Net_begin();
}

void loop()
{
    tryStartNetwork();

    lvgl_tick();
    Ui_tick();

    // Scheduler jobs (NM_SCHEDULER_OWN_TASK is 0, so they run here), the time
    // sync events and the serial console.
    tickNightMareESP();
    Net_loop();

    // LVGL asks to be pumped roughly every 5ms; anything less just spins.
    delay(5);
}
