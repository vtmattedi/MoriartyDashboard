#pragma once
#include <stdint.h>
#include <string>

#include "Sys.h"

// The cell in the enclosure, as read through the divider on GPIO 35.
//
// The board layer returns a voltage (lib/boardstuff, read_battery_mv); this
// turns it into the two things the panel shows -- a smoothed voltage and a
// percentage -- and publishes both as resources of the panel itself, `battery`
// and `battery_voltage`, so the rest of the network can see a panel running
// down without anyone walking over to it.
//
// The percentage is a lookup on resting voltage for a single lithium cell. It is
// an estimate and behaves like one: it sags while the backlight is at full and
// recovers when the panel rests, and it cannot tell a charging cell from a full
// one, since nothing on this board reports that a charger is attached.

/// @brief Take the first reading, declare the resources and schedule the
/// periodic sample. Call after board_init() and before startNightMareESP().
void Battery_begin();

/// @brief Read the divider now and fold it into the average. The periodic job
/// calls this; so does waking from sleep, where the last reading is stale by
/// however long the panel slept.
/// @param reset Take the new reading as it is instead of averaging it in.
void Battery_sample(bool reset = false);

/// @brief False when the divider reads too low for a cell to be there at all --
/// the panel on USB with nothing fitted.
bool Battery_present();

/// @brief Smoothed cell voltage, in millivolts.
uint16_t Battery_millivolts();

/// @brief Estimated charge, 0-100. Meaningless while Battery_present() is false.
uint8_t Battery_percent();

/// @brief True at or below the level worth warning about.
bool Battery_low();

/// @brief Changes whenever the percentage or presence does.
uint32_t Battery_revision();
