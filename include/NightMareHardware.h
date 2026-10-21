#pragma once

#include <NightMare/HardwareProfile.h>

namespace NMHardware
{

/*
 * Moriarty
 *
 * Physical hardware:
 *
 *     ESP32-3248S035C
 *
 * The complete unit is one market-board assembly. All represented devices are
 * physically mounted on that PCB, so all connections below are internal to
 * the same assembly.
 *
 * No connectors are required for the currently represented topology because
 * no connection leaves the board.
 *
 * Electrical nets are inferred by NightMare from the connection graph.
 */

inline Profile projectProfile()
{
    /*
     * =====================================================================
     * ESP32 MCU
     * =====================================================================
     */

    static const Terminal mcuTerminals[] = {
        // ST7796 TFT
        {"GPIO14"},
        {"GPIO13"},
        {"GPIO12"},
        {"GPIO15"},
        {"GPIO2"},
        {"GPIO27"},

        // GT911 touch
        {"GPIO33"},
        {"GPIO32"},
        {"GPIO25"},

        // microSD
        {"GPIO18"},
        {"GPIO23"},
        {"GPIO19"},
        {"GPIO5"},

        // RGB LED
        {"GPIO4"},
        {"GPIO16"},
        {"GPIO17"},

        // Light sensor
        {"GPIO34"},

        // Speaker
        {"GPIO26"},
    };

    /*
     * =====================================================================
     * ST7796 TFT
     * =====================================================================
     */

    static const Terminal displayTerminals[] = {
        {"SCLK"},
        {"MOSI"},
        {"MISO"},
        {"CS"},
        {"DC"},
        {"BL"},
    };

    /*
     * =====================================================================
     * GT911 capacitive touch controller
     * =====================================================================
     */

    static const Terminal touchTerminals[] = {
        {"SDA"},
        {"SCL"},
        {"RESET"},
    };

    /*
     * =====================================================================
     * microSD
     * =====================================================================
     */

    static const Terminal sdTerminals[] = {
        {"SCLK"},
        {"MOSI"},
        {"MISO"},
        {"CS"},
    };

    /*
     * =====================================================================
     * Onboard RGB LED
     * =====================================================================
     */

    static const Terminal rgbLedTerminals[] = {
        {"R"},
        {"G"},
        {"B"},
    };

    /*
     * =====================================================================
     * Onboard CDS light sensor
     * =====================================================================
     */

    static const Terminal lightSensorTerminals[] = {
        {"OUT"},
    };

    /*
     * =====================================================================
     * Onboard speaker
     * =====================================================================
     */

    static const Terminal speakerTerminals[] = {
        {"SIGNAL"},
    };

    /*
     * =====================================================================
     * Devices physically contained by the ESP32-3248S035C PCB
     * =====================================================================
     */

    static const Device devices[] = {
        {
            "mcu",
            mcuTerminals,
            sizeof(mcuTerminals) / sizeof(mcuTerminals[0]),
            "ESP32",
            "mcu",
            "ESP32",
        },

        {
            "display",
            displayTerminals,
            sizeof(displayTerminals) / sizeof(displayTerminals[0]),
            "TFT display",
            "display",
            "ST7796",
        },

        {
            "touch",
            touchTerminals,
            sizeof(touchTerminals) / sizeof(touchTerminals[0]),
            "Capacitive touch controller",
            "touch_controller",
            "GT911",
        },

        {
            "sd",
            sdTerminals,
            sizeof(sdTerminals) / sizeof(sdTerminals[0]),
            "microSD",
            "storage",
            "microSD",
        },

        {
            "rgb_led",
            rgbLedTerminals,
            sizeof(rgbLedTerminals) / sizeof(rgbLedTerminals[0]),
            "RGB LED",
            "led",
            "RGB LED",
        },

        {
            "light_sensor",
            lightSensorTerminals,
            sizeof(lightSensorTerminals) / sizeof(lightSensorTerminals[0]),
            "Light sensor",
            "sensor",
            "CDS",
        },

        {
            "speaker",
            speakerTerminals,
            sizeof(speakerTerminals) / sizeof(speakerTerminals[0]),
            "Speaker",
            "speaker",
            "Speaker",
        },
    };

    /*
     * =====================================================================
     * Internal PCB connections
     *
     * Definition-local assembly paths are empty because every endpoint below
     * belongs directly to the ESP32-3248S035C assembly itself.
     * =====================================================================
     */

    static const Connection internalConnections[] = {

        /*
         * ---------------------------------------------------------------
         * ESP32 -> ST7796 TFT
         *
         * GPIO14 -> SCLK
         * GPIO13 -> MOSI
         * GPIO12 -> MISO
         * GPIO15 -> CS
         * GPIO2  -> DC
         * GPIO27 -> BL
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO14"},
            {"", EndpointKind::DeviceTerminal, "display", "SCLK"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO13"},
            {"", EndpointKind::DeviceTerminal, "display", "MOSI"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO12"},
            {"", EndpointKind::DeviceTerminal, "display", "MISO"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO15"},
            {"", EndpointKind::DeviceTerminal, "display", "CS"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO2"},
            {"", EndpointKind::DeviceTerminal, "display", "DC"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO27"},
            {"", EndpointKind::DeviceTerminal, "display", "BL"},
        },

        /*
         * ---------------------------------------------------------------
         * ESP32 -> GT911
         *
         * GPIO33 -> SDA
         * GPIO32 -> SCL
         * GPIO25 -> RESET
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO33"},
            {"", EndpointKind::DeviceTerminal, "touch", "SDA"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO32"},
            {"", EndpointKind::DeviceTerminal, "touch", "SCL"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO25"},
            {"", EndpointKind::DeviceTerminal, "touch", "RESET"},
        },

        /*
         * ---------------------------------------------------------------
         * ESP32 -> microSD
         *
         * GPIO18 -> SCLK
         * GPIO23 -> MOSI
         * GPIO19 -> MISO
         * GPIO5  -> CS
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO18"},
            {"", EndpointKind::DeviceTerminal, "sd", "SCLK"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO23"},
            {"", EndpointKind::DeviceTerminal, "sd", "MOSI"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO19"},
            {"", EndpointKind::DeviceTerminal, "sd", "MISO"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO5"},
            {"", EndpointKind::DeviceTerminal, "sd", "CS"},
        },

        /*
         * ---------------------------------------------------------------
         * ESP32 -> onboard RGB LED
         *
         * GPIO4  -> R
         * GPIO16 -> G
         * GPIO17 -> B
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO4"},
            {"", EndpointKind::DeviceTerminal, "rgb_led", "R"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO16"},
            {"", EndpointKind::DeviceTerminal, "rgb_led", "G"},
        },

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO17"},
            {"", EndpointKind::DeviceTerminal, "rgb_led", "B"},
        },

        /*
         * ---------------------------------------------------------------
         * ESP32 -> onboard CDS light sensor
         *
         * GPIO34 -> OUT
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO34"},
            {"", EndpointKind::DeviceTerminal, "light_sensor", "OUT"},
        },

        /*
         * ---------------------------------------------------------------
         * ESP32 -> onboard speaker
         *
         * GPIO26 -> SIGNAL
         * ---------------------------------------------------------------
         */

        {
            {"", EndpointKind::DeviceTerminal, "mcu", "GPIO26"},
            {"", EndpointKind::DeviceTerminal, "speaker", "SIGNAL"},
        },
    };

    /*
     * =====================================================================
     * Reusable physical board definition
     * =====================================================================
     */

    static const HardwareDefinition definitions[] = {
        {
            "esp32-3248s035c",
            AssemblyKind::MarketBoard,
            "ESP32-3248S035C",
            "ESP32-3248S035C",
            nullptr,

            {
                nullptr,
                0,

                devices,
                sizeof(devices) / sizeof(devices[0]),

                nullptr,
                0,

                internalConnections,
                sizeof(internalConnections) / sizeof(internalConnections[0]),
            },
        },
    };

    /*
     * =====================================================================
     * Moriarty deployment
     * =====================================================================
     *
     * There is only one physical assembly in this deployment and it is also
     * the assembly running the firmware.
     * =====================================================================
     */

    static const Assembly roots[] = {
        {
            "main",
            "esp32-3248s035c",
            "Moriarty",
        },
    };

    return {
        "main",

        definitions,
        sizeof(definitions) / sizeof(definitions[0]),

        roots,
        sizeof(roots) / sizeof(roots[0]),

        nullptr,
        0,
    };
}

} // namespace NMHardware