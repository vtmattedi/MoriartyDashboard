#include "SensorBindings.h"
#include "Registry.h"
#include <NightMare.h>

namespace
{
    struct Binding
    {
        String device;
        String resource;
        bool inverted;
    };

    Binding bindings[SENSOR_SLOT_COUNT];
    uint32_t revision = 1;

    struct SlotConfig
    {
        const char *deviceKey;
        const char *resourceKey;
        const char *invertKey; // nullptr when polarity is meaningless
        const char *label;
    };

    // The setting names predate the resource protocol -- "key" meant a sensor
    // key, and now names a resource -- and are kept as they are so a panel that
    // is already deployed does not silently lose its bindings on the upgrade.
    // The AC controller happens to use the same two names for its own door
    // reference, but that is its own settings store on its own device: setting
    // one here does not set the other.
    const SlotConfig Slots[SENSOR_SLOT_COUNT] = {
        {"door_device", "door_key", "door_invert", "door"},
        {"rest_device", "rest_key", nullptr, "rest"},
    };

    bool validSlot(SensorSlot slot)
    {
        return slot >= 0 && slot < SENSOR_SLOT_COUNT;
    }
}

void SensorBindings_load()
{
    for (int i = 0; i < SENSOR_SLOT_COUNT; ++i)
    {
        bindings[i].device = PersistentSettings.get(Slots[i].deviceKey, "");
        bindings[i].resource = PersistentSettings.get(Slots[i].resourceKey, "");
        bindings[i].inverted =
            Slots[i].invertKey && PersistentSettings.get(Slots[i].invertKey, "0") == "1";
    }
    revision++;
}

void SensorBindings_set(SensorSlot slot, const String &device, const String &resource, bool inverted)
{
    if (!validSlot(slot))
    {
        return;
    }

    bindings[slot].device = device;
    bindings[slot].resource = resource;
    bindings[slot].inverted = inverted;

    PersistentSettings.set(Slots[slot].deviceKey, device);
    PersistentSettings.set(Slots[slot].resourceKey, resource);
    if (Slots[slot].invertKey)
    {
        PersistentSettings.set(Slots[slot].invertKey, inverted ? "1" : "0");
    }
    revision++;
}

void SensorBindings_clear(SensorSlot slot)
{
    SensorBindings_set(slot, "", "", false);
}

bool SensorBindings_isBound(SensorSlot slot)
{
    return validSlot(slot) && bindings[slot].device.length() && bindings[slot].resource.length();
}

bool SensorBindings_matches(SensorSlot slot, const String &device, const String &resource)
{
    return SensorBindings_isBound(slot) && bindings[slot].device == device &&
           bindings[slot].resource == resource;
}

const String &SensorBindings_device(SensorSlot slot)
{
    static const String empty;
    return validSlot(slot) ? bindings[slot].device : empty;
}

const String &SensorBindings_resource(SensorSlot slot)
{
    static const String empty;
    return validSlot(slot) ? bindings[slot].resource : empty;
}

bool SensorBindings_inverted(SensorSlot slot)
{
    return validSlot(slot) && bindings[slot].inverted;
}

String SensorBindings_value(SensorSlot slot)
{
    if (!SensorBindings_isBound(slot))
    {
        return "";
    }
    return Registry_resourceValue(bindings[slot].device, bindings[slot].resource);
}

DoorState SensorBindings_doorState()
{
    if (!SensorBindings_isBound(SENSOR_SLOT_DOOR))
    {
        return DOOR_UNBOUND;
    }

    String value = SensorBindings_value(SENSOR_SLOT_DOOR);
    value.trim();
    value.toLowerCase();

    // The boolean codec's two spellings, and nothing else. Anything unexpected
    // is reported as unknown rather than guessed at: a wrong "closed" on a door
    // indicator is worse than an honest question mark.
    bool one;
    if (value == "1" || value == "true")
    {
        one = true;
    }
    else if (value == "0" || value == "false")
    {
        one = false;
    }
    else
    {
        return DOOR_UNKNOWN;
    }

    const bool open = bindings[SENSOR_SLOT_DOOR].inverted ? !one : one;
    return open ? DOOR_OPEN : DOOR_CLOSED;
}

const char *SensorBindings_label(SensorSlot slot)
{
    return validSlot(slot) ? Slots[slot].label : "";
}

uint32_t SensorBindings_revision()
{
    return revision;
}
