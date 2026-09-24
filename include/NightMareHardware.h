/*----------------------------------------------------------
 * NightMareHardware.h -- this board's physical self-description.
 *
 * NightMareNetwork picks this up through __has_include. The host board's model
 * appears in retained <device>/info; the topology is retained at
 * <device>/hardware and <device>/hardware/msgpack. Without this file the panel
 * announces itself as
 *
 *     board: unspecified
 *     connections: none
 *
 * which is a poor answer from the one device on the network whose whole job is
 * to show what everything else is.
 *
 * SOURCE OF TRUTH: lib/boardstuff/boardstuff.h. Every pin number below is
 * copied from a constant there, and the two have to be changed together. They
 * are not #included from it, deliberately: this header is read by the
 * *library's* build, and boardstuff.h pulls in LVGL and Wire, so including it
 * would drag the whole display stack into NightMareNetwork's translation units
 * to fetch a dozen integers. The board is fixed hardware and these numbers
 * change about never; that is the trade.
 *
 * TOPOLOGY VERSION 3. The shape changed underneath this file: a Connection is
 * no longer a pin with attributes hanging off it. A Net is now one
 * electrically continuous conductor and carries the electrical facts -- signal
 * type, bus, direction, pull, polarity -- while a Connection is one physical
 * segment between two endpoints that says only which net it belongs to.
 *
 * On this board every net happens to be a single segment, because the ESP32
 * and all six peripherals share one PCB: there is no cable, no second board
 * and nothing external. So each net below has exactly one connection, running
 * from a GPIO on the host board to a terminal on a part soldered beside it.
 * That is a property of this panel, not of the schema -- a net exists so a
 * conductor can survive being split across a harness, and here it never is.
 *
 * What a pin number still cannot say -- which lines are inverted, which are
 * unused, why the touch reset must be pulsed -- is in the comments below.
 *----------------------------------------------------------*/

#pragma once

#include <NightMare/HardwareProfile.h>

namespace NMHardware
{
inline Profile projectProfile()
{
    // One PCB. Every device here is soldered to it, so none of them is NoBoard
    // -- that is for a genuinely external discrete component, and nothing on
    // this panel is reachable without a soldering iron.
    static const Board boards[] = {
        {"main", "esp32-3248s035c:v1"},
    };
    enum : uint8_t
    {
        BoardMain = 0
    };

    // Positions, because devices, nets and buses are all addressed by index.
    enum : uint8_t
    {
        DevTft = 0,
        DevTouch,
        DevLed,
        DevLight,
        DevSpeaker,
        DevCard
    };
    // Buses are numbered per board, not per protocol: the panel and the card
    // slot are two separate SPI buses and must not share an index.
    enum : uint8_t
    {
        BusTftSpi = 0,
        BusTouchI2c,
        BusCardSpi
    };
    // Net positions, in declaration order. Each connection below cites one.
    enum : uint8_t
    {
        NetTftSclk = 0,
        NetTftMosi,
        NetTftMiso,
        NetTftCs,
        NetTftDc,
        NetTftBacklight,
        NetTouchSda,
        NetTouchScl,
        NetTouchReset,
        NetLedRed,
        NetLedGreen,
        NetLedBlue,
        NetLightLevel,
        NetSpeakerOut,
        NetCardCs,
        NetCardSclk,
        NetCardMosi,
        NetCardMiso
    };

    static const Device devices[] = {
        {"tft", "ST7796", BoardMain, DeviceKind::Display, "tft-lcd"},
        {"touch", "GT911", BoardMain, DeviceKind::Sensor, "capacitive-touch"},
        {"led", "RGB status LED", BoardMain, DeviceKind::Led, "smd-rgb"},
        {"light", "CDS photoresistor", BoardMain, DeviceKind::Sensor, "photoresistor"},
        // A two-pin header, not a fitted speaker: what is on the board is the
        // connector, and the speaker is whatever gets plugged into it.
        {"speaker", "speaker header", BoardMain, DeviceKind::Connector},
        {"card", "microSD slot", BoardMain, DeviceKind::Connector, "microsd"},
    };

    static const Net nets[] = {
        // --- ST7796 320x480 panel, on its own SPI bus ------------------------
        // The panel's reset is tied to the board reset -- there is no GPIO for
        // it, which is why platformio.ini sets TFT_RST=-1 -- so it has no net.
        {"tft_sclk", SignalType::SpiClock, BusTftSpi, Direction::Output},
        {"tft_mosi", SignalType::SpiMosi, BusTftSpi, Direction::Output},
        {"tft_miso", SignalType::SpiMiso, BusTftSpi, Direction::Input},
        {"tft_cs", SignalType::SpiChipSelect, BusTftSpi, Direction::Output,
         Pull::None, true},
        // Data/command select. Off the bus on purpose: SCLK, MOSI, MISO and CS
        // are the shared conductor group, and this is a control line to one
        // device that nothing else could ever join.
        {"tft_dc", SignalType::Gpio, NoBus, Direction::Output},
        // Driven by set_backlight() at 5kHz 8-bit, never by TFT_eSPI.
        {"tft_backlight", SignalType::Pwm, NoBus, Direction::Output},

        // --- GT911 capacitive touch, I2C -------------------------------------
        // Its own I2C bus (i2c_gt911), not the Arduino default. Slave 0x5D.
        // The pull-ups are on the board; their value is not published here
        // because it has not been measured, and a guess in a self-description
        // is worse than an absence. There is no interrupt net: boardstuff polls
        // the controller rather than wiring INT.
        {"touch_sda", SignalType::I2cData, BusTouchI2c, Direction::Bidirectional,
         Pull::ExternalUp},
        // Often misreported as the backlight pin for this board.
        {"touch_scl", SignalType::I2cClock, BusTouchI2c, Direction::Output,
         Pull::ExternalUp},
        // Pulsed low at boot: the GT911 samples its address straps as this is
        // released, and stays silent if it never is.
        {"touch_reset", SignalType::Gpio, NoBus, Direction::Output, Pull::None, true},

        // --- Status LED ------------------------------------------------------
        // Common anode, so every duty cycle here is inverted.
        {"led_red", SignalType::Pwm, NoBus, Direction::Output, Pull::None, true},
        {"led_green", SignalType::Pwm, NoBus, Direction::Output, Pull::None, true},
        {"led_blue", SignalType::Pwm, NoBus, Direction::Output, Pull::None, true},

        // --- Sensing and audio -----------------------------------------------
        // Divider on ADC1, 11dB for the full 0-3.3V range. The divider is not a
        // pull, so none is declared.
        {"light_level", SignalType::Analog, NoBus, Direction::Input},
        // Reaches the header through an amplifier. Untouched by this firmware,
        // so how it would be driven is not asserted.
        {"speaker_out", SignalType::Gpio, NoBus, Direction::Output},

        // --- microSD slot, a second SPI bus ----------------------------------
        // Wired on the board but untouched by this firmware, so these four are
        // free for anything that needs them.
        {"card_cs", SignalType::SpiChipSelect, BusCardSpi, Direction::Output,
         Pull::None, true},
        {"card_sclk", SignalType::SpiClock, BusCardSpi, Direction::Output},
        {"card_mosi", SignalType::SpiMosi, BusCardSpi, Direction::Output},
        {"card_miso", SignalType::SpiMiso, BusCardSpi, Direction::Input},
    };

    // Host GPIO on one end, the part's own terminal name on the other. The card
    // slot's terminals are its SD-mode names, which is what a pinout prints,
    // rather than the SPI role the net already records.
    static const Connection connections[] = {
        {{EndpointKind::Board, BoardMain, "GPIO14"}, {EndpointKind::Device, DevTft, "SCK"},
         NetTftSclk},
        {{EndpointKind::Board, BoardMain, "GPIO13"}, {EndpointKind::Device, DevTft, "SDI"},
         NetTftMosi},
        {{EndpointKind::Board, BoardMain, "GPIO12"}, {EndpointKind::Device, DevTft, "SDO"},
         NetTftMiso},
        {{EndpointKind::Board, BoardMain, "GPIO15"}, {EndpointKind::Device, DevTft, "CS"},
         NetTftCs},
        {{EndpointKind::Board, BoardMain, "GPIO2"}, {EndpointKind::Device, DevTft, "DC"},
         NetTftDc},
        {{EndpointKind::Board, BoardMain, "GPIO27"}, {EndpointKind::Device, DevTft, "BL"},
         NetTftBacklight},

        {{EndpointKind::Board, BoardMain, "GPIO33"}, {EndpointKind::Device, DevTouch, "SDA"},
         NetTouchSda},
        {{EndpointKind::Board, BoardMain, "GPIO32"}, {EndpointKind::Device, DevTouch, "SCL"},
         NetTouchScl},
        {{EndpointKind::Board, BoardMain, "GPIO25"}, {EndpointKind::Device, DevTouch, "RST"},
         NetTouchReset},

        {{EndpointKind::Board, BoardMain, "GPIO4"}, {EndpointKind::Device, DevLed, "R"},
         NetLedRed},
        {{EndpointKind::Board, BoardMain, "GPIO16"}, {EndpointKind::Device, DevLed, "G"},
         NetLedGreen},
        {{EndpointKind::Board, BoardMain, "GPIO17"}, {EndpointKind::Device, DevLed, "B"},
         NetLedBlue},

        {{EndpointKind::Board, BoardMain, "GPIO34"}, {EndpointKind::Device, DevLight, "OUT"},
         NetLightLevel},
        {{EndpointKind::Board, BoardMain, "GPIO26"}, {EndpointKind::Device, DevSpeaker, "+"},
         NetSpeakerOut},

        {{EndpointKind::Board, BoardMain, "GPIO5"}, {EndpointKind::Device, DevCard, "DAT3"},
         NetCardCs},
        {{EndpointKind::Board, BoardMain, "GPIO18"}, {EndpointKind::Device, DevCard, "CLK"},
         NetCardSclk},
        {{EndpointKind::Board, BoardMain, "GPIO23"}, {EndpointKind::Device, DevCard, "CMD"},
         NetCardMosi},
        {{EndpointKind::Board, BoardMain, "GPIO19"}, {EndpointKind::Device, DevCard, "DAT0"},
         NetCardMiso},
    };

    return {BoardMain,
            boards, sizeof(boards) / sizeof(boards[0]),
            devices, sizeof(devices) / sizeof(devices[0]),
            nets, sizeof(nets) / sizeof(nets[0]),
            connections, sizeof(connections) / sizeof(connections[0])};
}
}
