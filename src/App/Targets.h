#pragma once
#include <Arduino.h>

// Which devices -- and which of their resources -- this panel drives.
//
// The dashboard owns no hardware of its own. Under the NightMare resource
// protocol every capability on the network has an address of the form
//
//     <device>/resource/<resource>
//
// so a role is bound to a pair, not to a device. The pair is runtime
// configuration, persisted in the NightMare settings store, so a panel can be
// re-pointed from the screen or the console without a reflash.
//
// Two shapes of binding, because the roles genuinely differ:
//
//   AC     one device, many resources. The AC controller publishes a whole
//          contract -- ac_power, ac_temperature, ac_target, ac_state,
//          ac_manual_sync, ac_sleep -- under names it chose, so binding the
//          role means naming the device and letting App/AcClient.h address the
//          rest. Targets_resource() is empty for this slot.
//
//   LIGHT  one resource each: a writable boolean and a writable 24-bit colour.
//   RGB    Nothing fixes their names, so both halves of the address are bound,
//          picked from the resources the panel has actually seen published --
//          the Resources screen is the chooser.
//
// Nothing here talks to the network. App/AcClient.cpp and App/LightControl.cpp
// point their Remote resources at whatever is bound, in Net_loop(), whenever
// Targets_revision() moves.
//
// Threading: the bindings are Strings, read by loop() several times a second to
// draw and to address a publish. Everything that writes them must therefore run
// on the loop() task -- the screens do. A console command does not: one arriving
// over MQTT is executed on the MQTT client's task, so it goes through
// Targets_request() and is applied a few milliseconds later by Net_loop().

enum TargetSlot
{
    TARGET_SLOT_AC = 0,
    TARGET_SLOT_LIGHT,
    TARGET_SLOT_RGB,
    TARGET_SLOT_COUNT
};

/// @brief Load the bindings from the settings store. Call after
/// introNightMareESP(), which is what opens it.
void Targets_load();

/// @brief Bind a slot. An empty device unbinds it. `resource` is ignored for
/// TARGET_SLOT_AC, whose resource names come from the AC contract.
/// Loop task only.
void Targets_set(TargetSlot slot, const String &device, const String &resource);

/// @brief Ask for a binding change from another task. One request is held at a
/// time and applied by Targets_applyPending(); a second one arriving before the
/// first has been applied is refused rather than queued, because the only
/// caller is a person typing at a console.
/// @param keepResource True to keep the slot's current resource, as
/// Targets_setDevice() does.
/// @return False when a request is already waiting.
bool Targets_request(TargetSlot slot, const String &device, const String &resource,
                     bool keepResource);

/// @brief Apply a pending request, if there is one. Loop task only; Net_loop()
/// calls it.
void Targets_applyPending();

/// @brief Bind a slot to a device, keeping whatever resource it already had (or
/// the slot's default). The Devices screen binds this way; the Resources screen
/// uses the two-argument form. Loop task only.
void Targets_setDevice(TargetSlot slot, const String &device);

/// @brief The device bound to a slot, or "" when unbound.
const String &Targets_device(TargetSlot slot);

/// @brief The resource bound to a slot. Always "" for TARGET_SLOT_AC.
const String &Targets_resource(TargetSlot slot);

/// @brief Whether a slot has a device bound to it.
bool Targets_configured(TargetSlot slot);

/// @brief Whether this exact (device, resource) pair fills the slot.
bool Targets_matches(TargetSlot slot, const String &device, const String &resource);

/// @brief Human-readable slot name ("AC", "Light", "RGB").
const char *Targets_label(TargetSlot slot);

/// @brief The resource name a slot falls back to when only a device is named.
const char *Targets_defaultResource(TargetSlot slot);

/// @brief Which slot a device fills, or -1. Roles are not exclusive by device --
/// one board may own an air conditioner and a colour LED both -- so this
/// reports the first, in slot order, and exists for the one-line badge on the
/// Devices list rather than as an answer to "what is this device for".
int Targets_slotOf(const String &device);

/// @brief Whether this device is bound as the air conditioner.
bool Targets_isAc(const String &device);

/// @brief Every binding as JSON, into a caller-supplied buffer. Safe from any
/// task, unlike the accessors above: it copies out of a fixed buffer that the
/// loop task rewrites in place, so a read during a change can see a mixture of
/// old and new text but never a String that has just been freed. 256 bytes is
/// enough for the full document.
void Targets_summaryJson(char *out, size_t size);

/// @brief Bumped whenever a binding changes, so the UI knows to redraw and
/// Net_loop() knows to re-point the remote resources.
uint32_t Targets_revision();
