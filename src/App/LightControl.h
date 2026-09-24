#pragma once
#include <Arduino.h>

// The two single-resource roles: a plain light and a colour light.
//
//   TARGET_SLOT_LIGHT   a writable boolean resource   RemoteState<bool>
//   TARGET_SLOT_RGB     a writable 24-bit colour      RemoteState<uint32_t>
//
// Neither has a contract the way the AC does, so both halves of the address are
// bound from the Resources screen -- pick the resource, give it the job. The
// library owns the subscription, the decoding, the freshness and the optimistic
// window; this module is only the panel's reading of what the value means.
//
// What the panel gets from the library for free, and used to hand-roll with a
// ServerVariable: the card flips the instant it is tapped, the owner's retained
// state wins again when it arrives, and a write the device never confirms
// reverts on its own once the optimistic window closes.
//
// # Brightness, out of one number
//
// A colour resource is one 24-bit value, so "off", "which colour" and "how
// bright" all have to live in it. They are read back out as:
//
//   off         the published value is 0
//   brightness  the largest of its three channels
//   colour      the value scaled back up so that channel reads 255
//
// The hue and the brightness last chosen are also kept here, so switching the
// light off and on again restores what it was rather than guessing, and so the
// colour wheel opens on the colour that was picked rather than on a dimmed
// version of it.

enum LightState
{
    LIGHT_UNBOUND = 0, // no resource bound to the light role
    LIGHT_UNKNOWN,     // bound, but the owner has reported nothing yet
    LIGHT_ON,
    LIGHT_OFF
};

/// @brief Declare both resources to the resource manager. Call once, before
/// startNightMareESP().
void LightControl_begin();

/// @brief Point both resources at what is currently bound. Called from
/// Net_loop() whenever Targets_revision() moves.
void LightControl_applyTargets();

/// @brief Copy both current values into the registry.
///
/// The resource manager consumes the state of every resource the panel has
/// bound, so it never reaches Net.cpp's generic message path and never lands in
/// the registry on its own -- and a light would then vanish from the Resources
/// screen the moment it was given a job, taking the only way to unbind it with
/// it. Called from Net_loop().
void LightControl_mirrorToRegistry();

#pragma region "Light"

LightState LightControl_state();

/// @brief Ask the bound device for the other state.
/// @return False if nothing is bound or the broker would not take it.
bool LightControl_toggle();

#pragma endregion

#pragma region "RGB"

/// @brief Whether the owner has reported a colour at all.
bool Rgb_hasState();

/// @brief Whether it is lit, i.e. the published colour is not black.
bool Rgb_on();

/// @brief The chosen colour at full brightness, for the swatch and the wheel.
uint32_t Rgb_colour();

/// @brief 0-255, derived from the published colour's largest channel.
uint8_t Rgb_brightness();

bool Rgb_setColour(uint32_t hex);
bool Rgb_setBrightness(uint8_t level);
bool Rgb_setOn(bool on);

#pragma endregion
