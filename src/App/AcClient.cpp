#include "AcClient.h"
#include "Targets.h"

#include <NightMare.h>

namespace
{
    // Resource names as the AC controller publishes them. Fixed rather than
    // bound per-resource like the light and the colour: the AC is not one
    // capability but a contract of nine, and asking someone to pair each of them
    // by hand would be a worse question than "which device is the AC".
    const char *const ResState = "ac_state";
    const char *const ResKnown = "ac_known";
    const char *const ResSleepDeadline = "ac_sleep_deadline";
    const char *const ResRoomTemperature = "temperature";
    const char *const ResPower = "ac_power";
    const char *const ResUnitTemperature = "ac_temperature";
    const char *const ResTarget = "ac_target";
    const char *const ActManualSync = "ac_manual_sync";
    const char *const ActSleep = "ac_sleep";

    // The target the thermostat comes back to when it has never had one. The
    // controller's own default; used only so the first tap on Auto has
    // something sensible to enable.
    const float DefaultTarget = 24.0f;

    // Static storage: the resource manager keeps non-owning pointers to these
    // for the life of the panel.
    RemoteSensor<int8_t> acState;
    RemoteSensor<bool> acKnown;
    RemoteSensor<uint32_t> acSleepDeadline;
    RemoteSensor<float> acRoomTemperature;
    RemoteState<bool> acPower;
    RemoteState<uint8_t> acUnitTemperature;
    RemoteState<float> acTarget;

    // Declared as schemas the panel expects, not as a claim to mirror the
    // controller's: a mismatch is logged by the library and changes nothing.
    const ActionArgMetadata ManualSyncArgs[] = {
        {"power", NetValueType::BOOLEAN},
        {"temperature", NetValueType::INTEGER},
    };
    const ActionArgMetadata SleepArgs[] = {
        {"minutes", NetValueType::INTEGER},
    };

    RemoteAction acManualSync(String(), ManualSyncArgs,
                              sizeof(ManualSyncArgs) / sizeof(ManualSyncArgs[0]));
    RemoteAction acSleep(String(), SleepArgs, sizeof(SleepArgs) / sizeof(SleepArgs[0]));

    /// The last device every resource was pointed at, so a revision bump that
    /// changed some other role does not tear down subscriptions needlessly --
    /// setSource() drops everything learned from the old source, including
    /// retained state that would then have to be replayed.
    String appliedDevice;
    bool applied = false;

    bool bound()
    {
        return Targets_configured(TARGET_SLOT_AC);
    }

    /// True once the controller has said anything at all. Freshness lives on
    /// `ac_state`, which the controller republishes on every change, so it is
    /// the one resource worth measuring the whole card against.
    bool hasState()
    {
        return acState.hasValue() && !acState.isStale();
    }

    int clampUnit(int temperature)
    {
        if (temperature < AC_UNIT_TEMP_MIN)
        {
            return AC_UNIT_TEMP_MIN;
        }
        return temperature > AC_UNIT_TEMP_MAX ? AC_UNIT_TEMP_MAX : temperature;
    }

    float clampTarget(float target)
    {
        if (target < (float)AC_UNIT_TEMP_MIN)
        {
            return (float)AC_UNIT_TEMP_MIN;
        }
        return target > (float)AC_UNIT_TEMP_MAX ? (float)AC_UNIT_TEMP_MAX : target;
    }
}

void AcClient_begin()
{
    // Bound before a source is known: the manager registers them now and
    // subscribes as soon as AcClient_applyTargets() supplies an address. Binding
    // this early also matters for identity cleanup, which can only withdraw
    // resources the firmware has already declared.
    gResourcesManager.bindResource(&acState);
    gResourcesManager.bindResource(&acKnown);
    gResourcesManager.bindResource(&acSleepDeadline);
    gResourcesManager.bindResource(&acRoomTemperature);
    gResourcesManager.bindResource(&acPower);
    gResourcesManager.bindResource(&acUnitTemperature);
    gResourcesManager.bindResource(&acTarget);
    gResourcesManager.bindResource(&acManualSync);
    gResourcesManager.bindResource(&acSleep);
}

void AcClient_applyTargets()
{
    const String &device = Targets_device(TARGET_SLOT_AC);
    if (applied && device == appliedDevice)
    {
        return;
    }
    appliedDevice = device;
    applied = true;

    // An empty device detaches each resource: it then matches no topic, keeps
    // its registration, and can be pointed somewhere valid later.
    acState.setSource(device, ResState);
    acKnown.setSource(device, ResKnown);
    acSleepDeadline.setSource(device, ResSleepDeadline);
    acRoomTemperature.setSource(device, ResRoomTemperature);
    acPower.setSource(device, ResPower);
    acUnitTemperature.setSource(device, ResUnitTemperature);
    acTarget.setSource(device, ResTarget);
    acManualSync.setSource(device, ActManualSync);
    acSleep.setSource(device, ActSleep);
}

#pragma region "Reading"

bool AcClient_isStale()
{
    return !bound() || !hasState();
}

AcState AcClient_state()
{
    if (!hasState())
    {
        return AC_UNKNOWN;
    }
    // The controller may believe the unit's state is unknown even while its own
    // state machine reports a value, so that is reported as unknown too.
    if (acKnown.hasValue() && !acKnown.getValue())
    {
        return AC_UNKNOWN;
    }
    return (AcState)acState.getValue();
}

bool AcClient_powerOn()
{
    return acPower.hasValue() && acPower.getValue();
}

int AcClient_unitTemperature()
{
    return acUnitTemperature.hasValue() ? (int)acUnitTemperature.getValue() : 0;
}

bool AcClient_targetEnabled()
{
    return acTarget.hasValue() && acTarget.getValue() >= 0.0f;
}

float AcClient_target()
{
    if (!acTarget.hasValue())
    {
        return 0.0f;
    }
    const float value = acTarget.getValue();
    return value < 0 ? -value : value;
}

bool AcClient_roomTemperature(float &out)
{
    // The controller only publishes `temperature` when it owns the sensor. On a
    // board whose reference is another device's reading, the resource is remote
    // there too and never appears under the controller's name -- so an absent
    // value is normal, not a fault, and the card simply shows nothing.
    if (!acRoomTemperature.hasValue() || acRoomTemperature.isStale())
    {
        return false;
    }
    out = acRoomTemperature.getValue();
    return true;
}

uint32_t AcClient_sleepDeadline()
{
    return acSleepDeadline.hasValue() ? acSleepDeadline.getValue() : 0;
}

#pragma endregion

#pragma region "Commanding"

bool AcClient_togglePower()
{
    if (!bound())
    {
        return false;
    }
    return acPower.setValue(!AcClient_powerOn());
}

bool AcClient_toggleTarget()
{
    if (!bound())
    {
        return false;
    }
    // Negating keeps the magnitude, which is how the controller remembers the
    // target to resume at. With nothing reported yet there is nothing to negate,
    // so the first tap enables the controller's own default.
    const float current = acTarget.hasValue() ? acTarget.getValue() : -DefaultTarget;
    return acTarget.setValue(-current);
}

bool AcClient_stepTarget(float delta)
{
    if (!bound())
    {
        return false;
    }
    const float magnitude = clampTarget(AcClient_target() > 0 ? AcClient_target() + delta
                                                              : DefaultTarget + delta);
    // Stepping adjusts the target without switching the thermostat on or off,
    // so the sign is carried over rather than recomputed.
    return acTarget.setValue(AcClient_targetEnabled() ? magnitude : -magnitude);
}

bool AcClient_stepUnitTemperature(int delta)
{
    if (!bound())
    {
        return false;
    }
    const int current = AcClient_unitTemperature();
    const int next = clampUnit((current > 0 ? current : (AC_UNIT_TEMP_MIN + AC_UNIT_TEMP_MAX) / 2) + delta);
    return acUnitTemperature.setValue((uint8_t)next);
}

bool AcClient_manualSync(int temperature, bool power)
{
    if (!bound())
    {
        return false;
    }
    char payload[64];
    snprintf(payload, sizeof(payload), "{\"power\":%s,\"temperature\":%d}",
             power ? "true" : "false", clampUnit(temperature));
    return acManualSync.invoke(payload);
}

bool AcClient_setSleepMinutes(uint32_t minutes)
{
    if (!bound())
    {
        return false;
    }
    char payload[48];
    snprintf(payload, sizeof(payload), "{\"minutes\":%lu}", (unsigned long)minutes);
    return acSleep.invoke(payload);
}

#pragma endregion
