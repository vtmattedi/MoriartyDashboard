#pragma once
#include <stdint.h>
#include <string>

#include "Sys.h"

// Firmware updates, pushed to the panel straight from the PC.
//
// Update mode is a small DashLink server -- the same wire protocol Dash-P4
// speaks (Dash-P4/pc/PROTOCOL.md): the panel listens on TCP 7340, the PC
// connects, a challenge answered with HMAC-SHA256 over a shared token proves
// the PC is the one paired, and then `python -m dashlink push-fw --target
// dashboard` streams the image into the idle OTA slot. Nothing goes through
// Dash-P4 and nothing is fetched from a web server.
//
// It is opt-in, not a service. Nothing listens until the Settings page or the
// OTA console command opens update mode, and it closes again on its own after
// two minutes without a PC, so an idle panel has no open port and pays no RAM
// for one. The one exception is the minute after booting into a fresh image,
// which the PC needs to ask "which build are you?" -- see below.
//
// Rollback is on. A freshly written image boots as "pending verify" and is
// marked good about 20 s after the main loop is running; one that crashes or
// resets before that is thrown out by the bootloader and the previous image
// runs again. The PC reads the build identity (the ELF sha256) from the
// challenge of the next session and compares it with the file it sent, so it
// can tell "updated" from "rolled back".
//
// What stops an update: a battery below OTA_MIN_BATTERY_PERCENT (a cell that
// dies halfway through a write leaves the old image, but a restart loop on a
// flat cell is nobody's idea of an upgrade), no pairing token compiled in, and
// no Wi-Fi station address. While update mode is open the panel does not rest
// into sleep, since sleep takes the radio down.

enum class OtaState : uint8_t
{
    Off,       // not listening
    Listening, // update mode is open, waiting for the PC
    Linked,    // a PC is connected and paired
    Receiving, // image bytes are arriving
    Verifying, // all bytes are in; checking the image
    Done,      // written, restarting
    Failed     // the last attempt failed; reason in Ota_message()
};

/// The charge below which a push is refused.
constexpr uint8_t OTA_MIN_BATTERY_PERCENT = 30;

/// @brief Check whether this boot is a fresh image still on probation, and if
/// so open update mode for the PC's verification. Call after Net_begin().
void Ota_begin();

/// @brief Main-loop housekeeping: marks the running image good once it has
/// proved itself, and closes update mode when it times out. Cheap.
void Ota_tick();

/// @brief Open update mode. False, with the reason in `why`, if it cannot.
bool Ota_start(std::string *why = nullptr);

/// @brief Close update mode. A transfer that is running is aborted and the old
/// firmware stays. Returns at once; the listener winds down by itself.
void Ota_stop();

/// @brief True from the moment update mode opens until it is closed. This is
/// what keeps the panel awake.
bool Ota_active();

OtaState Ota_state();

/// @brief 0-100 while receiving.
uint8_t Ota_percent();

/// @brief One line for the Settings page and the console: what update mode is
/// doing, or why the last attempt failed.
std::string Ota_message();

/// @brief Changes whenever the state or the percentage does, so the screens
/// can redraw without polling the text.
uint32_t Ota_revision();
