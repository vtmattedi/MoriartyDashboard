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
// NightMare device, watches every device and resource through the gateway, and drives
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
/// Device and resource names keep their original casing on purpose: topic
/// segments are case-sensitive, so "Micro" and "micro" are two different
/// devices.
static NightMareResults handleTargetCommand(const NightMareMessage &message)
{
    NightMareResults res;

    if (!message.subcommand.length())
    {
        // Use the snapshot so serial, ESP-NOW and UI callers share one path.
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

    // Staged rather than applied so every command source changes bindings at
    // the same point in Net_loop().
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
        // Largest free block matters as much as the total for LVGL and network
        // buffers. `internalFree` is what ESP.getFreeHeap() reports, IRAM
        // included; it is here for comparison.
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

/// Start after LVGL and the application resources exist. ESP-NOW has no TLS
/// handshake and therefore no longer needs the old 60KB/20KB MQTT heap gate.
static void tryStartNetwork()
{
    if (networkStarted)
    {
        return;
    }

    const size_t freeHeap = Heap_free();
    const size_t largestBlock = Heap_largest();
    Serial.printf("[net] starting with %u free / %u largest\n",
                  (unsigned)freeHeap, (unsigned)largestBlock);
    networkStarted = true;
    // Trace association, gateway discovery and the first subscription replay.
    Net_traceStartup();
    // Starts the shared radio, ESP-NOW, the Wi-Fi IP station and SNTP.
    startNightMareESP();
    // Claim the ESP-NOW message callback immediately, before a fast gateway can
    // replay the dashboard's wildcard subscriptions.
    Net_loop();
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
