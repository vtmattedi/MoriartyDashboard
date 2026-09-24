#include "Registry.h"
#include <NightMare.h>
#include <ArduinoJson.h>

namespace
{
    DeviceEntry devices[REGISTRY_MAX_DEVICES];
    int deviceCount = 0;

    ResourceEntry resources[REGISTRY_MAX_RESOURCES];
    int resourceCount = 0;

    uint32_t revision = 1;

    // Eviction is the failure this table can suffer without anything looking
    // wrong: the screens keep showing a full-looking list, it is simply a
    // different subset each time something new arrives. Counted, and announced
    // the first time, because silence here reads as "the network is small".
    int evictedDevices = 0;
    int evictedResources = 0;

    uint32_t stamp()
    {
        return (uint32_t)NightMare::Time::now();
    }

    /// Evict the entry heard from longest ago. Used only once a table is full,
    /// so a busy network pushes out stale devices instead of dropping new ones.
    int oldestDeviceIndex()
    {
        int oldest = 0;
        for (int i = 1; i < deviceCount; ++i)
        {
            if (devices[i].lastSeen < devices[oldest].lastSeen)
            {
                oldest = i;
            }
        }
        return oldest;
    }

    int oldestResourceIndex()
    {
        int oldest = 0;
        for (int i = 1; i < resourceCount; ++i)
        {
            if (resources[i].lastSeen < resources[oldest].lastSeen)
            {
                oldest = i;
            }
        }
        return oldest;
    }

    DeviceEntry *findDevice(const String &name)
    {
        for (int i = 0; i < deviceCount; ++i)
        {
            if (devices[i].name == name)
            {
                return &devices[i];
            }
        }
        return nullptr;
    }

    DeviceEntry *findOrCreateDevice(const String &name)
    {
        if (!name.length())
        {
            return nullptr;
        }

        DeviceEntry *existing = findDevice(name);
        if (existing)
        {
            return existing;
        }

        int slot;
        if (deviceCount < REGISTRY_MAX_DEVICES)
        {
            slot = deviceCount++;
        }
        else
        {
            slot = oldestDeviceIndex();
            ++evictedDevices;
            LOG_WARNING("REG", "Device table full at %d; evicting '%s' to make room for '%s'",
                        REGISTRY_MAX_DEVICES, devices[slot].name.c_str(), name.c_str());
        }

        // Serial.printf, not LOG(): the info level is compiled out at this
        // project's NM_LOG_LEVEL, and discovery is exactly what you need to see
        // when the screens disagree with the broker. Same channel as [heap].
        Serial.printf("[reg] +device   %s (%d/%d)\n", name.c_str(), deviceCount,
                      REGISTRY_MAX_DEVICES);
        devices[slot].name = name;
        devices[slot].hardware = "";
        devices[slot].online = true;
        devices[slot].lastSeen = stamp();
        revision++;
        return &devices[slot];
    }
}

void Registry_noteStatus(const String &device, const String &payload)
{
    // An empty retained payload is a tombstone -- the MQTT way of deleting a
    // retained announcement -- not a status of its own. It must never bring a
    // device into the table: the panel's own clear in Net_deleteDevice() comes
    // straight back on the "+/status" subscription, and creating on it would
    // resurrect the device it had just removed. An existing entry still goes
    // offline, which is what a tombstone from anyone else means.
    if (!payload.length())
    {
        DeviceEntry *entry = findDevice(device);
        if (entry && entry->online)
        {
            entry->online = false;
            revision++;
        }
        return;
    }

    // One shape, always: {"name":...,"hardware":...,"online":true}. The library
    // publishes it for going online, for a graceful shutdown and as the MQTT
    // last will, so every observer parses the same document.
    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok || !doc.is<JsonObject>())
    {
        return;
    }

    DeviceEntry *entry = findOrCreateDevice(device);
    if (!entry)
    {
        return;
    }

    // Absent "online" is treated as offline rather than as online: a malformed
    // or truncated document is not evidence that something is running.
    const bool online = doc["online"].as<bool>();
    if (entry->online != online)
    {
        entry->online = online;
        revision++;
    }

    JsonVariantConst hardware = doc["hardware"];
    if (hardware.is<const char *>())
    {
        const String signature = hardware.as<String>();
        if (entry->hardware != signature)
        {
            entry->hardware = signature;
            revision++;
        }
    }

    entry->lastSeen = stamp();
}

void Registry_noteSeen(const String &device)
{
    DeviceEntry *entry = findOrCreateDevice(device);
    if (entry)
    {
        entry->lastSeen = stamp();
    }
}

void Registry_noteResource(const String &device, const String &resource, const String &value)
{
    if (!device.length() || !resource.length())
    {
        return;
    }

    Registry_noteSeen(device);

    for (int i = 0; i < resourceCount; ++i)
    {
        if (resources[i].device == device && resources[i].resource == resource)
        {
            if (resources[i].value != value)
            {
                resources[i].value = value;
                revision++;
            }
            resources[i].lastSeen = stamp();
            return;
        }
    }

    int slot;
    if (resourceCount < REGISTRY_MAX_RESOURCES)
    {
        slot = resourceCount++;
    }
    else
    {
        slot = oldestResourceIndex();
        ++evictedResources;
        LOG_WARNING("REG", "Resource table full at %d; evicting '%s/%s' for '%s/%s'",
                    REGISTRY_MAX_RESOURCES, resources[slot].device.c_str(),
                    resources[slot].resource.c_str(), device.c_str(), resource.c_str());
    }

    Serial.printf("[reg] +resource %s/%s = %s (%d/%d)\n", device.c_str(), resource.c_str(),
                  value.c_str(), resourceCount, REGISTRY_MAX_RESOURCES);
    resources[slot].device = device;
    resources[slot].resource = resource;
    resources[slot].value = value;
    resources[slot].lastSeen = stamp();
    revision++;
}

int Registry_deviceCount()
{
    return deviceCount;
}

const DeviceEntry *Registry_device(int index)
{
    if (index < 0 || index >= deviceCount)
    {
        return nullptr;
    }
    return &devices[index];
}

const DeviceEntry *Registry_findDevice(const String &name)
{
    return findDevice(name);
}

void Registry_forgetDevice(const String &name)
{
    if (!name.length())
    {
        return;
    }

    // Close the gap by shifting the tail down rather than leaving a hole, so
    // index 0..count-1 stays dense -- the screens iterate it that way, and the
    // eviction scan assumes it too.
    for (int i = 0; i < deviceCount; ++i)
    {
        if (devices[i].name != name)
        {
            continue;
        }
        for (int j = i; j < deviceCount - 1; ++j)
        {
            devices[j] = devices[j + 1];
        }
        devices[deviceCount - 1] = DeviceEntry();
        deviceCount--;
        revision++;
        break;
    }

    // Its resources go too, or the Resources screen keeps listing a device the
    // Devices screen no longer shows.
    for (int i = resourceCount - 1; i >= 0; --i)
    {
        if (resources[i].device != name)
        {
            continue;
        }
        for (int j = i; j < resourceCount - 1; ++j)
        {
            resources[j] = resources[j + 1];
        }
        resources[resourceCount - 1] = ResourceEntry();
        resourceCount--;
        revision++;
    }
}

int Registry_resourceCount()
{
    return resourceCount;
}

const ResourceEntry *Registry_resource(int index)
{
    if (index < 0 || index >= resourceCount)
    {
        return nullptr;
    }
    return &resources[index];
}

String Registry_resourceValue(const String &device, const String &resource)
{
    for (int i = 0; i < resourceCount; ++i)
    {
        if (resources[i].device == device && resources[i].resource == resource)
        {
            return resources[i].value;
        }
    }
    return "";
}

int Registry_evictedDevices()
{
    return evictedDevices;
}

int Registry_evictedResources()
{
    return evictedResources;
}

void Registry_logSummary()
{
    Serial.printf("[reg] devices=%d/%d resources=%d/%d evicted=%d/%d\n", deviceCount,
                  REGISTRY_MAX_DEVICES, resourceCount, REGISTRY_MAX_RESOURCES, evictedDevices,
                  evictedResources);
}

void Registry_logAll()
{
    Registry_logSummary();
    for (int i = 0; i < deviceCount; ++i)
    {
        Serial.printf("[reg]   device  %-16s %-8s hw=%s\n", devices[i].name.c_str(),
                      devices[i].online ? "online" : "offline", devices[i].hardware.c_str());
    }
    for (int i = 0; i < resourceCount; ++i)
    {
        Serial.printf("[reg]   value   %s/%s = %s\n", resources[i].device.c_str(),
                      resources[i].resource.c_str(), resources[i].value.c_str());
    }
}

uint32_t Registry_revision()
{
    return revision;
}
