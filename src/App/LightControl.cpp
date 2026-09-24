#include "LightControl.h"
#include "Targets.h"
#include "Registry.h"

#include <NightMare.h>

namespace
{
    // Static storage: the resource manager keeps non-owning pointers to these
    // for the life of the panel.
    RemoteState<bool> light;
    RemoteState<uint32_t> rgb;

    String appliedLightDevice;
    String appliedLightResource;
    String appliedRgbDevice;
    String appliedRgbResource;
    bool applied = false;

    /// The colour and level last chosen here. Not the truth -- the device is --
    /// but what "on" should restore and what the wheel should open on, neither of
    /// which a published 0 can answer.
    uint32_t chosenColour = 0xFFFFFF;
    uint8_t chosenBrightness = 255;

    uint8_t channelMax(uint32_t colour)
    {
        const uint8_t r = (colour >> 16) & 0xFF;
        const uint8_t g = (colour >> 8) & 0xFF;
        const uint8_t b = colour & 0xFF;
        const uint8_t rg = r > g ? r : g;
        return rg > b ? rg : b;
    }

    /// Scale a colour so its largest channel reads 255, which is the hue with
    /// the brightness taken back out of it. Black has no hue to recover.
    uint32_t normalise(uint32_t colour)
    {
        const uint8_t peak = channelMax(colour);
        if (peak == 0)
        {
            return 0;
        }
        const uint32_t r = (((colour >> 16) & 0xFF) * 255u + peak / 2) / peak;
        const uint32_t g = (((colour >> 8) & 0xFF) * 255u + peak / 2) / peak;
        const uint32_t b = ((colour & 0xFF) * 255u + peak / 2) / peak;
        return (r << 16) | (g << 8) | b;
    }

    uint32_t scale(uint32_t colour, uint8_t level)
    {
        const uint32_t r = (((colour >> 16) & 0xFF) * level + 127) / 255;
        const uint32_t g = (((colour >> 8) & 0xFF) * level + 127) / 255;
        const uint32_t b = ((colour & 0xFF) * level + 127) / 255;
        return (r << 16) | (g << 8) | b;
    }

    uint32_t published()
    {
        return rgb.hasValue() ? (rgb.getValue() & 0x00FFFFFF) : 0;
    }

    /// Fold whatever the owner last reported back into the local choice, so the
    /// wheel and the slider follow a change made somewhere else -- another
    /// panel, a console, the device itself.
    void adoptPublished()
    {
        const uint32_t value = published();
        if (value == 0)
        {
            return; // Off says nothing about which colour or how bright.
        }
        chosenColour = normalise(value);
        chosenBrightness = channelMax(value);
    }

    bool publish(uint32_t colour)
    {
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            return false;
        }
        return rgb.setValue(colour & 0x00FFFFFF);
    }
}

void LightControl_begin()
{
    // Bound before a source is known: the manager registers them now and
    // subscribes as soon as LightControl_applyTargets() supplies an address.
    gResourcesManager.bindResource(&light);
    gResourcesManager.bindResource(&rgb);
}

void LightControl_applyTargets()
{
    const String &lightDevice = Targets_device(TARGET_SLOT_LIGHT);
    const String &lightResource = Targets_resource(TARGET_SLOT_LIGHT);
    const String &rgbDevice = Targets_device(TARGET_SLOT_RGB);
    const String &rgbResource = Targets_resource(TARGET_SLOT_RGB);

    // Each half is retargeted only when its own address moved: setSource() drops
    // everything learned from the old source, and a needless one would blank a
    // card until the broker replayed the retained state.
    if (!applied || lightDevice != appliedLightDevice || lightResource != appliedLightResource)
    {
        appliedLightDevice = lightDevice;
        appliedLightResource = lightResource;
        light.setSource(lightDevice, lightResource);
    }
    if (!applied || rgbDevice != appliedRgbDevice || rgbResource != appliedRgbResource)
    {
        appliedRgbDevice = rgbDevice;
        appliedRgbResource = rgbResource;
        rgb.setSource(rgbDevice, rgbResource);
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
    if (Targets_configured(TARGET_SLOT_RGB) && rgb.hasValue())
    {
        Registry_noteResource(Targets_device(TARGET_SLOT_RGB),
                              Targets_resource(TARGET_SLOT_RGB),
                              String(published()));
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
    return Targets_configured(TARGET_SLOT_RGB) && rgb.hasValue() && !rgb.isStale();
}

bool Rgb_on()
{
    return Rgb_hasState() && published() != 0;
}

uint32_t Rgb_colour()
{
    adoptPublished();
    return chosenColour;
}

uint8_t Rgb_brightness()
{
    adoptPublished();
    return chosenBrightness;
}

bool Rgb_setColour(uint32_t hex)
{
    const uint32_t hue = normalise(hex & 0x00FFFFFF);
    if (hue == 0)
    {
        // Picking black on the wheel means "off", not a colour of its own.
        return Rgb_setOn(false);
    }
    chosenColour = hue;
    return publish(scale(chosenColour, chosenBrightness));
}

bool Rgb_setBrightness(uint8_t level)
{
    chosenBrightness = level;
    // Moving the slider on a light that is off leaves it off, and sends nothing:
    // brightness is how bright it will be when it comes on, not a command to
    // switch it on. Reported as accepted, because the choice was taken.
    if (!Rgb_on())
    {
        return true;
    }
    return publish(scale(chosenColour, level));
}

bool Rgb_setOn(bool on)
{
    if (!on)
    {
        // Remember what it was before going dark, so the next "on" restores it.
        adoptPublished();
        return publish(0);
    }
    // Never switch on into black: a brightness of zero would look like the
    // command did nothing.
    const uint8_t level = chosenBrightness ? chosenBrightness : 255;
    chosenBrightness = level;
    return publish(scale(chosenColour, level));
}

#pragma endregion
