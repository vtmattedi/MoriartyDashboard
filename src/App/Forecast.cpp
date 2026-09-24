#include "Forecast.h"
#include "Net.h"
#include <ArduinoJson.h>
#include <NightMare.h>

// These two headers each define a function body, so they may be included from
// exactly one translation unit. This is that unit: weather_icons.h also drags
// in ~250KB of icon pixel data, and Forecast_icon() is the only way out.
#include <Weather icons/icons/convert.h>
#include <Weather icons/icons/weather_icons.h>

namespace
{
    ForecastData data;
    uint32_t revision = 1;

    /// Read the first of several accepted spellings that is actually present.
    bool readFloat(JsonDocument &doc, const char *const *keys, int keyCount, float &out)
    {
        for (int i = 0; i < keyCount; ++i)
        {
            JsonVariant v = doc[keys[i]];
            if (!v.isNull())
            {
                out = v.as<float>();
                return true;
            }
        }
        return false;
    }
}

void Forecast_request()
{
    Net_controlRequest("forecast");
}

bool Forecast_handle(const String &topic, const String &payload)
{
    if (topic != "Control/forecast")
    {
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok)
    {
        return true; // It was addressed to us; it was just unusable.
    }

    ForecastData next;

    JsonVariant icon = doc["icon"];
    if (!icon.isNull())
    {
        // Numeric icons are taken as amCharts ids directly; strings go through
        // the Visual Crossing mapping, which returns a negative id for the
        // conditions the icon set has no art for.
        if (icon.is<const char *>())
        {
            int mapped = visualCrossingToId(String(icon.as<const char *>()));
            next.iconId = mapped >= 0 ? mapped : -1;
        }
        else
        {
            next.iconId = icon.as<int>();
        }
    }

    static const char *tempKeys[] = {"temp", "temperature"};
    static const char *feelsKeys[] = {"feels", "feelslike"};
    static const char *minKeys[] = {"min", "tempmin"};
    static const char *maxKeys[] = {"max", "tempmax"};
    static const char *rainKeys[] = {"rain", "precipprob"};

    next.hasTemp = readFloat(doc, tempKeys, 2, next.temp);
    next.hasFeels = readFloat(doc, feelsKeys, 2, next.feels);

    float lo = 0, hi = 0;
    const bool hasMin = readFloat(doc, minKeys, 2, lo);
    const bool hasMax = readFloat(doc, maxKeys, 2, hi);
    next.hasRange = hasMin && hasMax;
    next.minTemp = lo;
    next.maxTemp = hi;

    float rain = 0;
    next.hasRain = readFloat(doc, rainKeys, 2, rain);
    next.rainProb = (int)(rain + 0.5f);

    JsonVariant condition = doc["condition"];
    if (condition.isNull())
    {
        condition = doc["conditions"];
    }
    if (!condition.isNull())
    {
        next.condition = condition.as<const char *>();
    }

    next.valid = next.hasTemp || next.iconId >= 0 || next.condition.length();
    next.updatedAt = (uint32_t)NightMare::Time::now();

    data = next;
    revision++;
    return true;
}

const ForecastData &Forecast_get()
{
    return data;
}

const lv_img_dsc_t *Forecast_icon()
{
    if (!data.valid || data.iconId < 0 || data.iconId >= WEATHER_ICONS_SIZE)
    {
        return nullptr;
    }
    return get_weather_icons((uint8_t)data.iconId);
}

uint32_t Forecast_revision()
{
    return revision;
}
