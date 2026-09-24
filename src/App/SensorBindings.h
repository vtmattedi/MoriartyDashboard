#pragma once
#include <Arduino.h>

// Resources on the network that this panel has been pointed at for a specific
// purpose. Chosen from a row on the Resources screen and persisted in the
// NightMare settings store, so they survive a reboot.
//
// Unlike the light and the colour, these two are read rather than driven, so
// they need no Remote resource of their own: the panel already sees every
// `<device>/resource/<name>/state` on the network and files it in the registry,
// and these are just a pointer into it.
//
// Two slots today:
//   DOOR -- drives the AC card's door indicator. Expected to read boolean
//           ("true"/"false" or "1"/"0"); nothing else is guessed at. Polarity is
//           part of the choice, because a reed switch reports 1 for "closed" or
//           for "open" depending only on how it was wired.
//   REST -- free slot shown in a corner of the resting screen. Any value, shown
//           verbatim.

enum SensorSlot
{
    SENSOR_SLOT_DOOR = 0,
    SENSOR_SLOT_REST,
    SENSOR_SLOT_COUNT
};

enum DoorState
{
    DOOR_UNBOUND = 0, // no resource chosen
    DOOR_UNKNOWN,     // chosen, but nothing published yet or not a boolean
    DOOR_OPEN,
    DOOR_CLOSED
};

/// @brief Load the bindings from the settings store. Call after
/// introNightMareESP().
void SensorBindings_load();

/// @brief Point a slot at a resource. `inverted` only means anything for the
/// door slot, where it flips the default reading of 1/true as "open".
void SensorBindings_set(SensorSlot slot, const String &device, const String &resource, bool inverted);

/// @brief Forget a slot's resource.
void SensorBindings_clear(SensorSlot slot);

bool SensorBindings_isBound(SensorSlot slot);

/// @brief Whether this is the resource in that slot.
bool SensorBindings_matches(SensorSlot slot, const String &device, const String &resource);

const String &SensorBindings_device(SensorSlot slot);
const String &SensorBindings_resource(SensorSlot slot);
bool SensorBindings_inverted(SensorSlot slot);

/// @brief The slot's latest value, read fresh from the registry. "" if unbound
/// or nothing has arrived.
String SensorBindings_value(SensorSlot slot);

/// @brief The door slot's value interpreted as a door state.
DoorState SensorBindings_doorState();

/// @brief Short label for a slot, for the Resources screen's badges.
const char *SensorBindings_label(SensorSlot slot);

/// @brief Bumped when any binding changes.
uint32_t SensorBindings_revision();
