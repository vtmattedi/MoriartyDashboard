#include "Targets.h"
#include "Sys.h"
#include <NightMare.h>
#include <atomic>

namespace
{
    uint32_t revision = 1;

    /// One staged request from another task. The fields are written before
    /// `pending` is set and read after it is seen, and the requesting task does
    /// not touch them again while it is set, so the two tasks never hold the
    /// same std::string at once.
    struct PendingRequest
    {
        int slot;
        std::string device;
        std::string resource;
        bool keepResource;
    };

    PendingRequest pendingRequest;
    std::atomic<bool> pending{false};

    struct SlotInfo
    {
        const char *deviceKey;
        const char *resourceKey; // nullptr when the slot binds a whole device
        const char *label;
        const char *defaultResource;
    };

    // Stored as two keys rather than one joined "device/resource": both halves
    // are single topic segments, but keeping them apart means nothing has to be
    // split back out, and the AC slot simply has no second key.
    const SlotInfo Slots[TARGET_SLOT_COUNT] = {
        {"ac_host", nullptr, "AC", ""},
        {"light_host", "light_res", "Light", "light"},
        {"rgb_host", nullptr, "RGB", ""},
        {"pc_host", nullptr, "PC", ""},
    };

    struct Binding
    {
        std::string device;
        std::string resource;
    };

    Binding bindings[TARGET_SLOT_COUNT];
    const std::string Empty;

    /// The bindings rendered as JSON, rewritten by the loop task on every
    /// change. It exists so the console's `TARGET` query has something it can
    /// read from the MQTT task: a fixed buffer read while it is being rewritten
    /// gives a mixture of old and new text, where copying the Strings directly
    /// could copy from one that has just been freed.
    char summary[256] = "{}";

    bool validSlot(TargetSlot slot)
    {
        return slot >= 0 && slot < TARGET_SLOT_COUNT;
    }

    /// Everything a slot stores, written together so a half-applied binding
    /// cannot survive a reboot.
    void persist(TargetSlot slot)
    {
        PersistentSettings.set(Slots[slot].deviceKey, bindings[slot].device);
        if (Slots[slot].resourceKey)
        {
            PersistentSettings.set(Slots[slot].resourceKey, bindings[slot].resource);
        }
    }

    /// Re-render the summary and announce the change. Every write path ends here.
    void bump()
    {
        int used = snprintf(summary, sizeof(summary), "{");
        for (int i = 0; i < TARGET_SLOT_COUNT && used < (int)sizeof(summary) - 2; ++i)
        {
            used += snprintf(summary + used, sizeof(summary) - used,
                             "%s\"%s\":{\"device\":\"%s\",\"resource\":\"%s\"}",
                             i ? "," : "", Slots[i].label, bindings[i].device.c_str(),
                             bindings[i].resource.c_str());
        }
        if (used < (int)sizeof(summary) - 2)
        {
            snprintf(summary + used, sizeof(summary) - used, "}");
        }
        revision++;
    }
}

void Targets_load()
{
    for (int i = 0; i < TARGET_SLOT_COUNT; ++i)
    {
        bindings[i].device = PersistentSettings.get(Slots[i].deviceKey, "");
        bindings[i].resource = Slots[i].resourceKey
                                   ? PersistentSettings.get(Slots[i].resourceKey,
                                                            Slots[i].defaultResource)
                                   : std::string();
    }
    bump();
}

void Targets_set(TargetSlot slot, const std::string &device, const std::string &resource)
{
    if (!validSlot(slot))
    {
        return;
    }

    std::string trimmedDevice = device;
    Text::trim(trimmedDevice);
    std::string trimmedResource = resource;
    Text::trim(trimmedResource);

    // Unbinding clears both halves: a resource name left behind would be
    // re-applied the next time a device is bound here, silently pointing the
    // role at something the user never chose.
    if (!trimmedDevice.length())
    {
        bindings[slot].device = "";
        bindings[slot].resource = Slots[slot].resourceKey ? std::string(Slots[slot].defaultResource)
                                                          : std::string();
        persist(slot);
        bump();
        return;
    }

    // Roles are deliberately not exclusive by device. Under the old protocol
    // each role owned a whole device, so binding one somewhere new had to clear
    // the old spot; a role is an address now, and one board can legitimately
    // fill several -- an AC controller with an RGB LED on it is a real device,
    // not a mistake to be corrected. The one thing that cannot be shared is a
    // single (device, resource) address, which the resource manager refuses on
    // its own rather than needing a rule here.
    bindings[slot].device = trimmedDevice;
    if (Slots[slot].resourceKey)
    {
        bindings[slot].resource = trimmedResource.length() ? trimmedResource
                                                           : std::string(Slots[slot].defaultResource);
    }
    persist(slot);
    bump();
}

void Targets_setDevice(TargetSlot slot, const std::string &device)
{
    if (!validSlot(slot))
    {
        return;
    }
    Targets_set(slot, device, bindings[slot].resource);
}

bool Targets_request(TargetSlot slot, const std::string &device, const std::string &resource,
                     bool keepResource)
{
    if (!validSlot(slot) || pending.load(std::memory_order_acquire))
    {
        return false;
    }
    pendingRequest.slot = slot;
    pendingRequest.device = device;
    pendingRequest.resource = resource;
    pendingRequest.keepResource = keepResource;
    pending.store(true, std::memory_order_release);
    return true;
}

void Targets_applyPending()
{
    if (!pending.load(std::memory_order_acquire))
    {
        return;
    }
    const TargetSlot slot = (TargetSlot)pendingRequest.slot;
    if (pendingRequest.keepResource)
    {
        Targets_setDevice(slot, pendingRequest.device);
    }
    else
    {
        Targets_set(slot, pendingRequest.device, pendingRequest.resource);
    }
    // Released only after the copies are done, so the requesting task cannot
    // start overwriting them while they are still being read.
    pendingRequest.device = "";
    pendingRequest.resource = "";
    pending.store(false, std::memory_order_release);
}

const std::string &Targets_device(TargetSlot slot)
{
    return validSlot(slot) ? bindings[slot].device : Empty;
}

const std::string &Targets_resource(TargetSlot slot)
{
    return validSlot(slot) ? bindings[slot].resource : Empty;
}

bool Targets_configured(TargetSlot slot)
{
    if (!validSlot(slot) || !bindings[slot].device.length())
    {
        return false;
    }
    // A resource slot with no resource name addresses nothing, so it is not
    // bound however much of a device name it holds.
    return !Slots[slot].resourceKey || bindings[slot].resource.length();
}

bool Targets_matches(TargetSlot slot, const std::string &device, const std::string &resource)
{
    if (!Targets_configured(slot))
    {
        return false;
    }
    if (bindings[slot].device != device)
    {
        return false;
    }
    return Slots[slot].resourceKey ? bindings[slot].resource == resource : true;
}

const char *Targets_label(TargetSlot slot)
{
    return validSlot(slot) ? Slots[slot].label : "";
}

const char *Targets_defaultResource(TargetSlot slot)
{
    return validSlot(slot) ? Slots[slot].defaultResource : "";
}

int Targets_slotOf(const std::string &device)
{
    if (!device.length())
    {
        return -1;
    }
    for (int i = 0; i < TARGET_SLOT_COUNT; ++i)
    {
        if (Targets_configured((TargetSlot)i) && bindings[i].device == device)
        {
            return i;
        }
    }
    return -1;
}

void Targets_summaryJson(char *out, size_t size)
{
    if (!out || !size)
    {
        return;
    }
    strncpy(out, summary, size - 1);
    out[size - 1] = '\0';
}

bool Targets_isAc(const std::string &device)
{
    return device.length() && Targets_configured(TARGET_SLOT_AC) &&
           bindings[TARGET_SLOT_AC].device == device;
}

uint32_t Targets_revision()
{
    return revision;
}
