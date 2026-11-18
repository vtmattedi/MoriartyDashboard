#include "LightControl.h"
#include "Sys.h"
#include "Targets.h"
#include "Registry.h"

#include <NightMare.h>

namespace
{
    // Static storage: the resource manager keeps non-owning pointers to these
    // for the life of the panel.
    RemoteState<bool> light{"light"};
    // The bed light is a whole device (Sherlock), addressed by two fixed resources:
    // `led:power` and `led:color`. Brightness is left to the device.
    RemoteState<bool> bedPower{"led:power"};
    RemoteState<ColourType> bedColour{"led:color"};
    const char *const BedPowerResource = "led:power";
    const char *const BedColourResource = "led:color";

    std::string appliedLightDevice;
    std::string appliedLightResource;
    std::string appliedBedDevice;
    bool applied = false;

    /// The colour last chosen here, so "on" and the wheel have something to show
    /// before the device has reported one.
    uint32_t chosenColour = 0xFFFFFF;

    uint32_t publishedColour()
    {
        const NightMare::RGB c = bedColour.getValue().toRGB();
        return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
    }
}

void LightControl_begin()
{
    // Bound before a source is known: the manager registers them now and
    // subscribes as soon as LightControl_applyTargets() supplies an address.
    gResourcesManager.bindResource(&light);
    gResourcesManager.bindResource(&bedPower);
    gResourcesManager.bindResource(&bedColour);
}

void LightControl_applyTargets()
{
    const std::string &lightDevice = Targets_device(TARGET_SLOT_LIGHT);
    const std::string &lightResource = Targets_resource(TARGET_SLOT_LIGHT);
    const std::string &bedDevice = Targets_device(TARGET_SLOT_RGB);

    // Each half is retargeted only when its own address moved: setSource() drops
    // everything learned from the old source, and a needless one would blank a
    // card until the broker replayed the retained state.
    if (!applied || lightDevice != appliedLightDevice || lightResource != appliedLightResource)
    {
        appliedLightDevice = lightDevice;
        appliedLightResource = lightResource;
        light.setSource(lightDevice, lightResource);
    }
    if (!applied || bedDevice != appliedBedDevice)
    {
        appliedBedDevice = bedDevice;
        bedPower.setSource(bedDevice, BedPowerResource);
        bedColour.setSource(bedDevice, BedColourResource);
    }
    applied = true;
}

void LightControl_mirrorToRegistry()
{
    if (Targets_configured(TARGET_SLOT_LIGHT) && light.hasValue())
    {
        Registry_noteResource(Targets_device(TARGET_SLOT_LIGHT),
                              Targets_resource(TARGET_SLOT_LIGHT),
                              light.getValue() ? "true" : "false");
    }
    if (Targets_configured(TARGET_SLOT_RGB))
    {
        if (bedPower.hasValue())
        {
            Registry_noteResource(Targets_device(TARGET_SLOT_RGB), BedPowerResource,
                                  bedPower.getValue() ? "true" : "false");
        }
        if (bedColour.hasValue())
        {
            Registry_noteResource(Targets_device(TARGET_SLOT_RGB), BedColourResource,
                                  std::to_string(publishedColour()));
        }
    }
}

#pragma region "Light"

LightState LightControl_state()
{
    if (!Targets_configured(TARGET_SLOT_LIGHT))
    {
        return LIGHT_UNBOUND;
    }
    // Stale means the owner withdrew its retained state; the last value is still
    // readable, but reporting it as fact would be a guess.
    if (!light.hasValue() || light.isStale())
    {
        return LIGHT_UNKNOWN;
    }
    return light.getValue() ? LIGHT_ON : LIGHT_OFF;
}

bool LightControl_toggle()
{
    if (!Targets_configured(TARGET_SLOT_LIGHT))
    {
        return false;
    }
    // With no reading yet, "the other state" is undefined -- asking for "on" is
    // the useful guess, since that is what someone tapping a dark card wants.
    const bool next = light.hasValue() ? !light.getValue() : true;
    return light.setValue(next);
}

#pragma endregion

#pragma region "RGB"

bool Rgb_hasState()
{
    return Targets_configured(TARGET_SLOT_RGB) && bedPower.hasValue() && !bedPower.isStale();
}

bool Rgb_on()
{
    return Rgb_hasState() && bedPower.getValue();
}

uint32_t Rgb_colour()
{
    if (bedColour.hasValue())
    {
        const uint32_t value = publishedColour();
        if (value != 0)
        {
            chosenColour = value;
        }
    }
    return chosenColour;
}

bool Rgb_setColour(uint32_t hex)
{
    hex &= 0x00FFFFFF;
    if (hex == 0)
    {
        // Picking black on the wheel means "off", not a colour of its own.
        return Rgb_setOn(false);
    }
    if (!Targets_configured(TARGET_SLOT_RGB))
    {
        return false;
    }
    chosenColour = hex;
    bool ok = bedColour.setValue(ColourType((uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex));
    if (!Rgb_on())
    {
        ok = bedPower.setValue(true) && ok;
    }
    return ok;
}

bool Rgb_setOn(bool on)
{
    return Targets_configured(TARGET_SLOT_RGB) && bedPower.setValue(on);
}

#pragma endregion
