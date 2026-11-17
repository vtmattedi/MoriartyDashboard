#pragma once
#include <stdint.h>
#include <string>

#include "Sys.h"

// What the panel has heard on the network.
//
// Under the resource protocol a device announces itself on a retained
// `<device>/status` and publishes each capability's value on a retained
// `<device>/resource/<name>/state`. The panel subscribes to `+/status` and
// `+/resource/+/state`, so on every reconnect the broker replays the whole
// network into here -- see App/Net.cpp.
//
// This keeps a bounded snapshot of both for the Devices and Resources screens --
// newest-wins, oldest evicted -- rather than growing without limit on a network
// whose size the panel does not control.

// Sized for what a panel can usefully retain, not for what the network might
// publish. The Resources screen pages through this table with four reusable
// rows so registry capacity no longer implies the same number of LVGL widgets.
//
// Devices are what the panel tracks all the time (it subscribes to `+/status`).
// Resources are not: the panel keeps those it is using (bound to a role), plus
// those of the one device whose page is open -- see Net_watchDevice(). A single
// AC controller publishes around eighteen, so this holds the AC's and one more
// device's with room over.
#define REGISTRY_MAX_DEVICES 32
#define REGISTRY_MAX_RESOURCES 40

struct DeviceEntry
{
    std::string name;
    /// The board behind the name, from the status document's "hardware" field.
    /// Empty until a status has been heard.
    std::string hardware;
    bool online;
    uint32_t lastSeen; // NightMare::Time::now(), seconds
};

struct ResourceEntry
{
    std::string device;
    std::string resource;
    /// The codec representation exactly as published: "true", "23.5", "cool".
    std::string value;
    uint32_t lastSeen; // NightMare::Time::now(), seconds
};

/// @brief Record a device's retained status document,
/// `{"name":...,"hardware":...,"online":true}`. An empty payload is a
/// tombstone: it takes an existing device offline but never creates one.
void Registry_noteStatus(const std::string &device, const std::string &payload);

/// @brief Record that a device was heard from, without changing its online flag.
void Registry_noteSeen(const std::string &device);

/// @brief Record one resource's published state.
void Registry_noteResource(const std::string &device, const std::string &resource, const std::string &value);

int Registry_deviceCount();
const DeviceEntry *Registry_device(int index);

/// @brief Forget the resources of `device` that `keep` does not want. Used when
/// its page is left: what the panel is not using is not worth the memory.
void Registry_dropResources(const std::string &device, bool (*keep)(const ResourceEntry &entry));

/// @brief Find a device by exact (case-sensitive) name, or nullptr.
const DeviceEntry *Registry_findDevice(const std::string &name);

/// @brief Drop a device and its resource states from the panel's memory.
///
/// Local only: it does not tell the device or the broker anything. The device
/// will reappear the moment it publishes again, which is the intended
/// behaviour -- see Net_deleteDevice() for the part that clears what the broker
/// retains so it stays gone.
void Registry_forgetDevice(const std::string &name);

int Registry_resourceCount();
const ResourceEntry *Registry_resource(int index);

/// @brief The latest published state of one resource, or "" if never seen.
std::string Registry_resourceValue(const std::string &device, const std::string &resource);

/// @brief Bumped on every change, so screens can redraw only when something moved.
uint32_t Registry_revision();

/// @brief How many entries have been pushed out of a full table since boot.
/// Non-zero means the panel is seeing more of the network than it can hold and
/// the screens are showing a rotating subset, not a stable one.
int Registry_evictedDevices();
int Registry_evictedResources();

/// @brief One summary line: occupancy of both tables and the eviction counts.
void Registry_logSummary();

/// @brief Every device and resource currently held, one per line. For the
/// console -- it is long, and on a busy network it is very long.
void Registry_logAll();
