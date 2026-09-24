#pragma once
#include <Arduino.h>

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
// A single AC controller publishes around eighteen resources on its own, so the
// resource table is deliberately several times the device table.
#define REGISTRY_MAX_DEVICES 16
#define REGISTRY_MAX_RESOURCES 40

struct DeviceEntry
{
    String name;
    /// The board behind the name, from the status document's "hardware" field.
    /// Empty until a status has been heard.
    String hardware;
    bool online;
    uint32_t lastSeen; // NightMare::Time::now(), seconds
};

struct ResourceEntry
{
    String device;
    String resource;
    /// The codec representation exactly as published: "true", "23.5", "cool".
    String value;
    uint32_t lastSeen; // NightMare::Time::now(), seconds
};

/// @brief Record a device's retained status document,
/// `{"name":...,"hardware":...,"online":true}`. An empty payload is a
/// tombstone: it takes an existing device offline but never creates one.
void Registry_noteStatus(const String &device, const String &payload);

/// @brief Record that a device was heard from, without changing its online flag.
void Registry_noteSeen(const String &device);

/// @brief Record one resource's published state.
void Registry_noteResource(const String &device, const String &resource, const String &value);

int Registry_deviceCount();
const DeviceEntry *Registry_device(int index);

/// @brief Find a device by exact (case-sensitive) name, or nullptr.
const DeviceEntry *Registry_findDevice(const String &name);

/// @brief Drop a device and its resource states from the panel's memory.
///
/// Local only: it does not tell the device or the broker anything. The device
/// will reappear the moment it publishes again, which is the intended
/// behaviour -- see Net_deleteDevice() for the part that clears what the broker
/// retains so it stays gone.
void Registry_forgetDevice(const String &name);

int Registry_resourceCount();
const ResourceEntry *Registry_resource(int index);

/// @brief The latest published state of one resource, or "" if never seen.
String Registry_resourceValue(const String &device, const String &resource);

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
