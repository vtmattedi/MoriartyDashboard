#pragma once
#include <Arduino.h>

// The air conditioner bound to TARGET_SLOT_AC.
//
// The AC controller publishes a fixed contract of NightMare resources, and this
// is the panel's client for it:
//
//   ac_state            int8    sensor  the controller's own verdict, below
//   ac_known            bool    sensor  whether it believes it knows the unit
//   ac_sleep_deadline   uint32  sensor  epoch second the unit switches off, 0 none
//   temperature         float   sensor  the room reference it controls against
//   ac_power            bool    state   the unit's power
//   ac_temperature      uint8   state   the unit's own setpoint, 18..30
//   ac_target           float   state   room target; negative disables the thermostat
//   ac_manual_sync      action  {"power":bool,"temperature":int}
//   ac_sleep            action  {"minutes":int}, 0 cancels
//
// Every one of them is declared without a source and pointed at the bound
// device by AcClient_applyTargets(), so re-binding the role is a retarget rather
// than a rebuild: the library drops what it learned from the old device,
// re-subscribes, and the retained state of the new one arrives on its own.
//
// Writes are optimistic. Tapping power flips the card immediately and the owner's
// retained `ac_power` state wins again once it arrives -- or once the optimistic
// window closes, which is how a command that went nowhere becomes visible.
//
// Threading: the resource values are plain scalars written by the MQTT task when
// owner state arrives and read by loop() to draw. Each is a single aligned load
// or store, so no reading here can tear. Nothing in this module is a String for
// that reason -- the controller's `ac_status` document is deliberately not
// consumed, because two tasks touching one String is what corrupts the heap.

/// The controller's own state machine, as published on `ac_state`.
enum AcState : int8_t
{
    AC_OFF_TARGET_DOOR_OPEN = -4,
    AC_OFF_DOOR_OPEN = -3,
    AC_OFF_TARGET = -2,
    AC_OFF = -1,
    AC_UNKNOWN = 0,
    AC_ON = 1,
    AC_ON_TARGET = 2,
};

/// The setpoint range the unit itself accepts.
#define AC_UNIT_TEMP_MIN 18
#define AC_UNIT_TEMP_MAX 30

/// @brief Declare the contract to the resource manager. Call once, before
/// startNightMareESP(), so a pending identity cleanup can see it.
void AcClient_begin();

/// @brief Point every resource at the currently bound device. Called from
/// Net_loop() whenever Targets_revision() moves.
void AcClient_applyTargets();

#pragma region "Reading"

/// @brief True when the controller has not reported recently, or never has.
/// The card shows "no data" rather than guessing at a state.
bool AcClient_isStale();

/// @brief The controller's verdict. AC_UNKNOWN until one has arrived.
AcState AcClient_state();

/// @brief Whether the unit is powered, as the controller last reported it.
bool AcClient_powerOn();

/// @brief The unit's own setpoint, or 0 when none has been reported.
int AcClient_unitTemperature();

/// @brief Whether the room-temperature thermostat is running. A negative
/// `ac_target` is how the controller reports it switched off, the magnitude
/// remembering the target to come back to.
bool AcClient_targetEnabled();

/// @brief The room target, always positive. 0 when none has been reported.
float AcClient_target();

/// @brief The room temperature the controller works from.
/// @return False when the controller has published none, or none recently.
bool AcClient_roomTemperature(float &out);

/// @brief Epoch second the unit is due to switch itself off, or 0 for no timer.
uint32_t AcClient_sleepDeadline();

#pragma endregion

#pragma region "Commanding"

/// Every one of these returns false when nothing was sent -- no device bound, or
/// the broker would not take it. None of them is an acknowledgement that the
/// controller accepted the request; that arrives as new owner state, or does not.

bool AcClient_togglePower();

/// @brief Switch the room thermostat on or off, keeping the target it held.
bool AcClient_toggleTarget();

/// @brief Move the room target, in degrees. Clamped to the unit's range.
bool AcClient_stepTarget(float delta);

/// @brief Move the unit's own setpoint, in whole degrees. Clamped likewise.
bool AcClient_stepUnitTemperature(int delta);

/// @brief Tell the controller what the physical unit is already doing, after it
/// was changed behind its back -- usually with the unit's own IR remote. Not a
/// command to the unit: it corrects a belief rather than changing the room.
bool AcClient_manualSync(int temperature, bool power);

/// @brief Arm the unit's own sleep timer. 0 cancels it.
bool AcClient_setSleepMinutes(uint32_t minutes);

#pragma endregion
