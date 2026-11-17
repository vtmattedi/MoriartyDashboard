#pragma once
#include <stdint.h>
#include <string>

#include "Sys.h"

// The PC, reached through the panel that is linked to it.
//
// This panel has no link to the PC for controlling it and is not getting one
// (App/Ota.h is a separate, opt-in link used only to push firmware): the PC talks to
// Dash-P4, over the LAN, and Dash-P4 is the only thing it talks to. What
// Dash-P4 does is offer that link to the network as three resources of its
// own, and this file is the other end of them:
//
//   pc_linked         bool sensor   a PC is paired with Dash-P4 and connected
//   pc_monitors_off   action        turn the PC's monitors off
//   pc_sleep          action        put the PC to sleep (suspend)
//
// So the binding is a device, like the AC's -- TARGET_SLOT_PC, set on the
// Devices page or with `TARGET PC <device>` -- and the three names are a
// contract that device publishes.
//
// An invoke is fire-and-forget, as every action on this panel is: true means
// the request was handed to the transport, not that the PC obeyed. `pc_linked`
// is the nearest thing to a confirmation -- after a sleep it drops to false
// within a few seconds, when Dash-P4 notices the PC has gone.

/// @brief Declare the resources. Call before startNightMareESP().
void PcClient_begin();

/// @brief Point the resources at whatever fills TARGET_SLOT_PC. Net_loop()
/// calls it when a binding changes.
void PcClient_applyTargets();

enum PcLink
{
    PC_LINK_UNKNOWN = 0, // bound, but the linking panel has not said either way
    PC_LINK_DOWN,        // the linking panel is up and has no PC connected
    PC_LINK_UP
};

/// @brief Whether the linking panel currently has a PC to pass a request to.
PcLink PcClient_link();

/// @brief Turn the PC's monitors off. False when unbound or nothing was sent.
bool PcClient_monitorsOff();

/// @brief Put the PC to sleep. False when unbound or nothing was sent.
bool PcClient_sleep();
