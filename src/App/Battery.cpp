#include "Battery.h"
#include "Sys.h"

#include <boardstuff.h>
#include <NightMare.h>

namespace
{
    // Below this there is no cell on the divider, whatever else is: a lithium
    // cell's protection circuit disconnects it long before it gets here.
    const uint16_t PresentAboveMv = 2500;
    const uint8_t LowAtPercent = 15;
    const uint32_t SampleIntervalMs = 10000;

    // Resting voltage against charge for one lithium cell. The curve is flat
    // through the middle and falls off a cliff at the end, which is why this is
    // a table and not a straight line between 3.3V and 4.2V -- a line reads 55%
    // at the voltage where a real cell has 20% left.
    struct CurvePoint
    {
        uint16_t millivolts;
        uint8_t percent;
    };
    const CurvePoint Curve[] = {
        {4200, 100}, {4110, 90}, {4020, 80}, {3950, 70}, {3870, 60}, {3840, 50},
        {3800, 40},  {3770, 30}, {3730, 20}, {3690, 10}, {3610, 5},  {3300, 0},
    };
    const int CurvePoints = sizeof(Curve) / sizeof(Curve[0]);

    ManagedSensor<uint8_t> batteryPercent("battery");
    ManagedSensor<float> batteryVoltage("battery_voltage");

    uint16_t millivolts = 0;
    uint8_t percent = 0;
    bool present = false;
    bool sampled = false;
    uint32_t revision = 0;

    // What the resources were last told, so a sample that changes nothing
    // publishes nothing.
    bool percentPublished = false;
    bool voltagePublished = false;
    uint16_t publishedMillivolts = 0;

    uint8_t percentFor(uint16_t mv)
    {
        if (mv >= Curve[0].millivolts)
        {
            return 100;
        }
        for (int i = 1; i < CurvePoints; ++i)
        {
            if (mv >= Curve[i].millivolts)
            {
                const CurvePoint &high = Curve[i - 1];
                const CurvePoint &low = Curve[i];
                return (uint8_t)(low.percent +
                                 ((uint32_t)(mv - low.millivolts) * (high.percent - low.percent)) /
                                     (high.millivolts - low.millivolts));
            }
        }
        return 0;
    }

    void sampleJob()
    {
        Battery_sample();
    }
}

void Battery_sample(bool reset)
{
    const uint16_t reading = read_battery_mv();

    // A quarter of each new reading. The divider is noisy and the load moves
    // the cell by tens of millivolts when the backlight changes; unsmoothed,
    // the percentage flickers between neighbours on every sample.
    if (reset || !sampled)
    {
        millivolts = reading;
    }
    else
    {
        millivolts = (uint16_t)(((uint32_t)millivolts * 3 + reading) / 4);
    }
    sampled = true;

    const bool nowPresent = millivolts >= PresentAboveMv;
    const uint8_t nowPercent = nowPresent ? percentFor(millivolts) : 0;
    const bool changed = nowPresent != present || nowPercent != percent;
    present = nowPresent;
    percent = nowPercent;

    if (changed)
    {
        ++revision;
    }

    // Published on a change worth hearing about, not on every sample: the
    // percentage when it moves, the voltage when it has drifted 20mV. With no
    // cell there is nothing to report, and the resources say so by being
    // unavailable rather than by reading zero.
    static bool availabilityTold = false;
    static bool availabilityWas = false;
    if (!availabilityTold || availabilityWas != present)
    {
        availabilityTold = true;
        availabilityWas = present;
        batteryPercent.setAvailable(present);
        batteryVoltage.setAvailable(present);
    }
    if (!present)
    {
        return;
    }
    if (changed || !percentPublished)
    {
        percentPublished = true;
        batteryPercent.setValue(percent);
    }
    const uint16_t drift = millivolts > publishedMillivolts ? millivolts - publishedMillivolts
                                                            : publishedMillivolts - millivolts;
    if (drift >= 20 || !voltagePublished)
    {
        voltagePublished = true;
        publishedMillivolts = millivolts;
        batteryVoltage.setValue(millivolts / 1000.0f);
    }
}

void Battery_begin()
{
    gResourcesManager.bindResource(&batteryPercent);
    gResourcesManager.bindResource(&batteryVoltage);

    Battery_sample(true);
    if (present)
    {
        printf("[battery] %u mV, %u%%\n", (unsigned)millivolts, (unsigned)percent);
    }
    else
    {
        printf("[battery] %u mV, no cell detected\n", (unsigned)millivolts);
    }

    gScheduler.everyMonotonic("dash.battery", sampleJob, SampleIntervalMs);
}

bool Battery_present()
{
    return present;
}

uint16_t Battery_millivolts()
{
    return millivolts;
}

uint8_t Battery_percent()
{
    return percent;
}

bool Battery_low()
{
    return present && percent <= LowAtPercent;
}

uint32_t Battery_revision()
{
    return revision;
}
