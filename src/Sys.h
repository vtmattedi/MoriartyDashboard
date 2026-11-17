#pragma once

// The few things every file here used to get from the Arduino core, under their
// own names now that there is no Arduino core: a millisecond clock, a sleep
// that yields to the scheduler, and a clamp.

#include <stdint.h>
#include <stdio.h>
#include <string>

#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <Core/Text.h>

// trim, toLower, startsWith, indexOf, slice, replaceAll, toLong ... -- the
// library's std::string helpers, under a shorter name.
namespace Text = NightMare::Text;

/// The degree sign, as the UTF-8 bytes the built-in LVGL fonts draw. Concatenate it as its own literal,
/// e.g. "%.1f" DEG -- a hex escape written inline runs on into a following hex digit.
#define DEG "\xC2\xB0"

/// Milliseconds since boot. Wraps after about 49 days, so compare by
/// subtraction, never by magnitude. It keeps counting across light sleep.
inline uint32_t millisNow()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/// Block this task and let the others run.
inline void sleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

inline long clampTo(long value, long low, long high)
{
    return value < low ? low : (value > high ? high : value);
}
