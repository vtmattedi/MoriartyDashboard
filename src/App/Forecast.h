#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Weather for the focus screen.
//
// The panel has no weather API key and no business holding one: it asks the
// NightMare backend, over the same Control/request channel the time sync uses.
// The panel publishes "forecast" to `Control/request` and the backend answers
// on `Control/forecast` with:
//
//   {"icon":"partly-cloudy-day","temp":24.1,"feels":25.0,
//    "min":19.2,"max":29.8,"rain":20,"condition":"Partly cloudy"}
//
// `icon` is a Visual Crossing icon name, mapped to the bundled amCharts icon
// set by visualCrossingToId() in include/Weather icons/icons/convert.h. Every
// field is optional: whatever is missing simply does not render. Longer field
// spellings (feelslike / precipprob / conditions) are accepted too, so the
// backend can emit a Visual Crossing payload more or less unchanged.

struct ForecastData
{
    bool valid = false;
    int iconId = -1;
    bool hasTemp = false;
    float temp = 0;
    bool hasFeels = false;
    float feels = 0;
    bool hasRange = false;
    float minTemp = 0;
    float maxTemp = 0;
    bool hasRain = false;
    int rainProb = 0;
    String condition;
    uint32_t updatedAt = 0; // now(), seconds
};

/// @brief Ask the backend for a fresh forecast.
void Forecast_request();

/// @brief Feed a Control/* message in. Returns true if it was a forecast reply.
bool Forecast_handle(const String &topic, const String &payload);

const ForecastData &Forecast_get();

/// @brief The icon for the current forecast, or nullptr when there is nothing to show.
const lv_img_dsc_t *Forecast_icon();

/// @brief Bumped when new weather arrives.
uint32_t Forecast_revision();
