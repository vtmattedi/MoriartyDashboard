#include <stdint.h>
#include <string>

#include "Sys.h"
#include <boardstuff.h>
#include "nvs_flash.h"
#include <NightMare.h>

#include "App/Net.h"
#include "App/Heap.h"
#include "App/Registry.h"
#include "App/Targets.h"
#include "App/Forecast.h"
#include "App/SensorBindings.h"
#include "App/AcClient.h"
#include "App/LightControl.h"
#include "App/PcClient.h"
#include "App/Battery.h"
#include "App/Ota.h"
#include "UI/Ui.h"

// NightMare Network dashboard, handheld: the panel in a battery enclosure.
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
///   TARGET PC Dash-P4               bind the PC role to the panel linked to the PC
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
        if (Text::equalsIgnoreCase(message.subcommand, Targets_label((TargetSlot)i)))
        {
            slot = i;
            break;
        }
    }

    if (slot < 0)
    {
        res.result = false;
        res.response = "Unknown role. Expected one of: AC, LIGHT, RGB, PC.";
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
                         haveResource ? message.args[2] : std::string(""), !haveResource))
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
    res.response = std::string("{\"device\":\"") + message.args[1] + "\",\"resource\":\"" +
                   (haveResource ? message.args[2] : std::string(Targets_defaultResource((TargetSlot)slot))) +
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
        // buffers. `internalFree` is what heap_caps_get_free_size(MALLOC_CAP_INTERNAL) reports, IRAM
        // included; it is here for comparison.
        res.response = std::string("{\"free\":") + std::to_string(Heap_free()) +
                       ",\"minFree\":" + std::to_string(Heap_minFree()) +
                       ",\"largestBlock\":" + std::to_string(Heap_largest()) +
                       ",\"internalFree\":" + std::to_string(Heap_freeInternal()) +
                       ",\"droppedMessages\":" + std::to_string(Net_droppedMessages()) +
                       ",\"filteredMessages\":" + std::to_string(Net_filteredMessages()) + "}";
        return res;
    }

    if (message.command == "BATTERY")
    {
        NightMareResults res;
        res.result = true;
        res.response = std::string("{\"present\":") + (Battery_present() ? "true" : "false") +
                       ",\"millivolts\":" + std::to_string(Battery_millivolts()) +
                       ",\"percent\":" + std::to_string(Battery_percent()) + "}";
        return res;
    }

    if (message.command == "SLEEP")
    {
        // Staged, like TARGET: sleeping blocks loop() until the glass is
        // touched, and that must not happen inside a command handler that
        // still owes its caller a reply.
        Ui_sleepNow();
        NightMareResults res;
        res.result = true;
        res.response = "Sleeping. Touch the screen to wake.";
        return res;
    }

    if (message.command == "OTA")
    {
        // OTA            the state of update mode
        // OTA START      open update mode (also on the Settings page)
        // OTA STOP       close it
        NightMareResults res;
        res.result = true;
        const std::string action = message.subcommand;
        if (Text::equalsIgnoreCase(action, "START"))
        {
            std::string why;
            res.result = Ota_start(&why);
            if (!res.result)
            {
                res.response = why;
                return res;
            }
        }
        else if (Text::equalsIgnoreCase(action, "STOP"))
        {
            Ota_stop();
        }
        res.response = std::string("{\"active\":") + (Ota_active() ? "true" : "false") +
                       ",\"status\":\"" + Ota_message() + "\"}";
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
        res.response = std::string("{\"devices\":") + std::to_string(Registry_deviceCount()) +
                       ",\"resources\":" + std::to_string(Registry_resourceCount()) +
                       ",\"evictedDevices\":" + std::to_string(Registry_evictedDevices()) +
                       ",\"evictedResources\":" + std::to_string(Registry_evictedResources()) +
                       ",\"maxDevices\":" + std::to_string(REGISTRY_MAX_DEVICES) +
                       ",\"maxResources\":" + std::to_string(REGISTRY_MAX_RESOURCES) + "}";
        return res;
    }

    NightMareResults res;
    res.result = false;
    res.response = "Unknown command. Available: [TARGET, FORECAST, HEAP, BATTERY, SLEEP, OTA, REGISTRY]. "
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
    printf("[net] starting with %u free / %u largest\n",
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

static void setup()
{
    // Before anything that starts the radio: the Wi-Fi PHY keeps its RF
    // calibration in NVS, and without it every start does a full calibration
    // (slower, and noticeable on every wake from sleep). The Arduino core did
    // this itself. NightMare's settings are a separate SPIFFS partition.
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

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
    PcClient_begin();
    // The panel's only resources of its own, so the same rule applies to them.
    Battery_begin();
    Net_begin();
    Ota_begin();
}

/// One pass of everything the panel does. All of it on this one task: LVGL, the
/// screens, the Scheduler's jobs (NM_SCHEDULER_OWN_TASK is 0) and the console
/// share application state with no locking, and that is only sound because
/// nothing else touches it.
static void loop()

{
    tryStartNetwork();

    lvgl_tick();
    Ui_tick();

    // Scheduler jobs (NM_SCHEDULER_OWN_TASK is 0, so they run here), the time
    // sync events and the serial console.
    tickNightMareESP();
    Net_loop();
    Ota_tick();

    // LVGL asks to be pumped roughly every 5ms; anything less just spins.
    sleepMs(5);
}

extern "C" void app_main(void)
{
    setup();
    for (;;)
    {
        loop();
    }
}
