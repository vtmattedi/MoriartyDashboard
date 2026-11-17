#include "PcClient.h"
#include "Sys.h"
#include "Targets.h"

#include <NightMare.h>

namespace
{
    const char *const ResLinked = "pc_linked";
    const char *const ActMonitorsOff = "pc_monitors_off";
    const char *const ActSleep = "pc_sleep";

    // A scalar, like everything else this panel binds: the library stores
    // incoming state from its own task, and a bool read from loop() cannot tear.
    RemoteSensor<bool> pcLinked{ResLinked};
    RemoteAction pcMonitorsOff{std::string(ActMonitorsOff)};
    RemoteAction pcSleep{std::string(ActSleep)};

    std::string appliedDevice;
    bool applied = false;
}

void PcClient_begin()
{
    gResourcesManager.bindResource(&pcLinked);
    gResourcesManager.bindResource(&pcMonitorsOff);
    gResourcesManager.bindResource(&pcSleep);
}

void PcClient_applyTargets()
{
    const std::string &device = Targets_device(TARGET_SLOT_PC);
    if (applied && device == appliedDevice)
    {
        return;
    }
    appliedDevice = device;
    applied = true;

    // An empty device detaches each resource, as in AcClient_applyTargets().
    pcLinked.setSource(device, ResLinked);
    pcMonitorsOff.setSource(device, ActMonitorsOff);
    pcSleep.setSource(device, ActSleep);
}

PcLink PcClient_link()
{
    if (!Targets_configured(TARGET_SLOT_PC) || !pcLinked.hasValue() || pcLinked.isStale())
    {
        return PC_LINK_UNKNOWN;
    }
    return pcLinked.getValue() ? PC_LINK_UP : PC_LINK_DOWN;
}

bool PcClient_monitorsOff()
{
    return Targets_configured(TARGET_SLOT_PC) && pcMonitorsOff.invoke();
}

bool PcClient_sleep()
{
    return Targets_configured(TARGET_SLOT_PC) && pcSleep.invoke();
}
