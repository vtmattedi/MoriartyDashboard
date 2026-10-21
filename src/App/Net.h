#pragma once
#include <Arduino.h>

// The panel's link to the rest of the NightMare network.
//
// Two separate paths in, and the difference matters:
//
//   * The things the panel *drives* are NightMare resources, declared in
//     App/AcClient.h and App/LightControl.h. The library owns their
//     subscriptions, decoding, freshness and optimistic writes; nothing in this
//     file touches them beyond re-pointing them when a binding changes.
//
//   * The things the panel *watches* -- every device on the network and every
//     resource value it publishes -- arrive through three explicit wildcard
//     subscriptions installed here:
//
//         +/status                 who is out there, and whether it is up
//         +/resource/+/state       what each of them currently reads
//         Control/forecast         the weather, answered by the backend
//
//     The library no longer takes "#", so these are the panel's own, and they
//     persist across gateway reconnects. Anything the resource manager recognises as
//     belonging to a bound resource is consumed before it reaches here, which is
//     why App/LightControl.h mirrors those few values back into the registry.
//
// ESP-NOW messages arrive on the Wi-Fi event task but are processed on loop(),
// handed across through a fixed-size ring buffer -- see Net.cpp.

/// @brief Install the ESP-NOW ingress bridge, subscriptions and periodic
/// jobs. Call after the resource clients have been declared.
void Net_begin();

/// @brief Drain the inbox, re-point the resource clients after a binding change
/// and time out any MQTTP request. Call from loop().
void Net_loop();

/// @brief Publish a request on the shared Control/request channel.
void Net_controlRequest(const char *what);

/// @brief Ask every NightMare device to republish its MessagePack manifest.
/// @return False while the NightMare gateway is disconnected.
bool Net_requestAllManifests();

/// @brief Tell a device to restart, over its console topic.
/// @return False if the gateway is down or the name is empty.
bool Net_rebootDevice(const String &deviceName);

/// @brief Remove a device from the network's view of itself.
///
/// Drops it from the panel's memory, unbinds it from any role, and clears the
/// retained announcements that would otherwise bring it back on the next gateway
/// reconnect: its status, its resource manifest and the state of every resource
/// the panel has seen it publish. It does not stop the device -- anything still
/// running and publishing will simply reappear, which is the point. This clears
/// the leftovers of something that is gone; it does not decommission something
/// that is not.
///
/// A resource the panel never heard cannot be tombstoned, for the same reason
/// the library's own identity cleanup can only reach resources it has declared.
/// @return False if the gateway is down or the name is empty.
bool Net_deleteDevice(const String &deviceName);

/// @brief Wanted messages lost because the inbox was full or oversize.
/// Should stay at 0; a rising count means loop() is too slow to keep up.
uint32_t Net_droppedMessages();

/// @brief Log heap every half second through radio, Wi-Fi and gateway startup.
void Net_traceStartup();
