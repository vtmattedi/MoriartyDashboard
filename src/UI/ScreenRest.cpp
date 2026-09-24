#include "Screens.h"
#include "Theme.h"

#include "../App/Net.h"
#include "../App/Targets.h"
#include "../App/Forecast.h"
#include "../App/SensorBindings.h"
#include "../App/AcClient.h"
#include "../App/LightControl.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <WiFi.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The resting screen: what the panel looks like when nobody is using it.
//
// A dimmed mark and a clock big enough to read across the room, with the corners
// carrying what is worth knowing at a glance:
//
//   top-left      anything wrong -- door open, no WiFi, no broker
//   top-right     temperatures, outside and in the room
//   bottom-left   a resource of your choosing (pick it on the Resources screen)
//   bottom-right  what is switched on
//
// Any touch is swallowed by the manager to wake the panel, so nothing here is
// interactive.

LV_IMG_DECLARE(logo_mw);
// Montserrat Medium at 120px, digits only -- see the header of the generated
// file for why and how to regenerate it.
LV_FONT_DECLARE(font_clock_120);

namespace
{
    lv_obj_t *overlay = nullptr;
    lv_obj_t *hoursLabel = nullptr;
    lv_obj_t *colonLabel = nullptr;
    lv_obj_t *minutesLabel = nullptr;
    lv_obj_t *clockRow = nullptr;
    lv_obj_t *dateLabel = nullptr;

    lv_obj_t *cornerWarnings = nullptr;
    lv_obj_t *cornerTemps = nullptr;
    lv_obj_t *cornerSensor = nullptr;
    lv_obj_t *cornerState = nullptr;

    // Last values drawn. This screen can stay up all night, and re-setting a
    // label's text redraws it even when the text is the same, so the large
    // digits are only touched when a digit actually changes.
    int shownHour = -1;
    int shownMinute = -1;
    int shownColon = -1;

    const lv_coord_t CornerWidth = 190;
    const lv_coord_t CornerInset = 12;

    const char *const WeekdayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
    const char *const MonthNames[] = {"January", "February", "March", "April", "May", "June",
                                      "July", "August", "September", "October", "November", "December"};

    /// Append to a fixed buffer, tracking the real length written.
    /// snprintf returns what it *would* have written, so adding its result
    /// straight onto the offset can push it past the end of the buffer and make
    /// the next "remaining" calculation underflow.
    void appendf(char *buffer, size_t size, size_t &used, const char *format, ...)
    {
        if (used >= size - 1)
        {
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(buffer + used, size - used, format, args);
        va_end(args);

        if (written < 0)
        {
            return;
        }
        used += (size_t)written < (size - used) ? (size_t)written : (size - used - 1);
    }

    void setColonVisible(bool visible)
    {
        if (shownColon == (int)visible)
        {
            return;
        }
        shownColon = visible;
        lv_obj_set_style_text_opa(colonLabel, visible ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    }

    /// One corner: a small wrapped, recolourable label.
    lv_obj_t *makeCorner(lv_obj_t *parent, lv_align_t align, lv_coord_t dx, lv_coord_t dy,
                         lv_text_align_t textAlign)
    {
        lv_obj_t *label = Theme_label(parent, "", &lv_font_montserrat_16, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_recolor(label, true);
        lv_obj_set_width(label, CornerWidth);
        lv_obj_set_style_text_align(label, textAlign, LV_PART_MAIN);
        lv_obj_set_style_text_line_space(label, 3, LV_PART_MAIN);
        lv_obj_align(label, align, dx, dy);
        return label;
    }

    /// Values come from device names and sensor keys, which cannot contain '#'
    /// (it is an MQTT wildcard, not allowed in a published topic) -- but a
    /// reading can, so anything interpolated is stripped of the recolour marker.
    String safe(const String &raw)
    {
        String out = raw;
        out.replace("#", "*");
        return out;
    }

    /// The wall clock, broken down in the process timezone. The library's own
    /// hour()/minute()/day() are deliberately UTC, which is the wrong thing for
    /// a clock someone reads across the room.
    tm localNow(time_t epoch)
    {
        tm parts{};
        localtime_r(&epoch, &parts);
        return parts;
    }
}

lv_obj_t *ScreenRest_create(lv_obj_t *parent)
{
    overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(overlay, UI_COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    // Clickable so touches land here rather than on the page underneath; the
    // manager turns the touch into a wake instead of a control press.
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *logo = lv_img_create(overlay);
    lv_img_set_src(logo, &logo_mw);
    lv_obj_center(logo);
    lv_obj_set_style_img_opa(logo, LV_OPA_20, LV_PART_MAIN);

    // Hours, colon and minutes as three labels, not one "HH:MM" string. The
    // blink is done by fading the colon rather than by swapping it for a space
    // the way LIVE_TIME_STR does: a space is narrower than a colon, so a centred
    // single label would shift sideways every second -- invisible at 16px,
    // obvious at 120px. Same rule for when it blinks, though: hidden on even
    // seconds.
    clockRow = lv_obj_create(overlay);
    Theme_plainContainer(clockRow);
    lv_obj_clear_flag(clockRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(clockRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clockRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clockRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(clockRow, LV_ALIGN_CENTER, 0, -14);

    hoursLabel = Theme_label(clockRow, "--", &font_clock_120, UI_COL_TEXT);
    colonLabel = Theme_label(clockRow, ":", &font_clock_120, UI_COL_TEXT);
    minutesLabel = Theme_label(clockRow, "--", &font_clock_120, UI_COL_TEXT);

    dateLabel = Theme_label(overlay, "", &lv_font_montserrat_16, UI_COL_TEXT_DIM);
    lv_obj_align_to(dateLabel, clockRow, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    cornerWarnings = makeCorner(overlay, LV_ALIGN_TOP_LEFT, CornerInset, CornerInset, LV_TEXT_ALIGN_LEFT);
    cornerTemps = makeCorner(overlay, LV_ALIGN_TOP_RIGHT, -CornerInset, CornerInset, LV_TEXT_ALIGN_RIGHT);
    cornerSensor = makeCorner(overlay, LV_ALIGN_BOTTOM_LEFT, CornerInset, -CornerInset, LV_TEXT_ALIGN_LEFT);
    cornerState = makeCorner(overlay, LV_ALIGN_BOTTOM_RIGHT, -CornerInset, -CornerInset, LV_TEXT_ALIGN_RIGHT);

    return overlay;
}

void ScreenRest_refresh()
{
    if (!NightMare::Time::valid())
    {
        Theme_setText(hoursLabel, "--");
        Theme_setText(minutesLabel, "--");
        setColonVisible(true);
        shownHour = shownMinute = -1;
        Theme_setText(dateLabel, "waiting for time sync");
    }
    else
    {
        const time_t epoch = NightMare::Time::now();
        const tm parts = localNow(epoch);
        char digits[4];

        if (parts.tm_hour != shownHour)
        {
            shownHour = parts.tm_hour;
            snprintf(digits, sizeof(digits), "%02d", parts.tm_hour);
            lv_label_set_text(hoursLabel, digits);
        }
        if (parts.tm_min != shownMinute)
        {
            shownMinute = parts.tm_min;
            snprintf(digits, sizeof(digits), "%02d", parts.tm_min);
            lv_label_set_text(minutesLabel, digits);
        }

        // The same rule the library's OnlyTimeLive format uses.
        setColonVisible((epoch & 1) != 0);

        char date[48];
        snprintf(date, sizeof(date), "%s, %d %s",
                 WeekdayNames[parts.tm_wday], parts.tm_mday, MonthNames[parts.tm_mon]);
        Theme_setText(dateLabel, date);
    }

    // The date sits under the clock, and the clock row's width follows its digits
    // ("11" is narrower than "20"), so keep them lined up.
    lv_obj_align_to(dateLabel, clockRow, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    char buffer[160];
    size_t used = 0;

    // Top left: only things that are wrong. Empty when all is well, which is the
    // point -- anything in this corner means attention.
    buffer[0] = '\0';
    used = 0;
    if (SensorBindings_doorState() == DOOR_OPEN)
    {
        appendf(buffer, sizeof(buffer), used, "#ffc64b " LV_SYMBOL_WARNING " Door open#");
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        appendf(buffer, sizeof(buffer), used, "%s#f2566a " LV_SYMBOL_WIFI " No WiFi#", used ? "\n" : "");
    }
    else if (!MQTT_Connected())
    {
        // Only worth saying when the WiFi is up: otherwise it is the same fault
        // reported twice.
        appendf(buffer, sizeof(buffer), used, "%s#f2566a " LV_SYMBOL_LOOP " No broker#", used ? "\n" : "");
    }
    Theme_setText(cornerWarnings, buffer);

    // Top right: temperatures.
    buffer[0] = '\0';
    used = 0;
    const ForecastData &weather = Forecast_get();
    if (weather.hasTemp)
    {
        appendf(buffer, sizeof(buffer), used, "#8a97a8 outside#  %.1f\xC2\xB0", weather.temp);
    }
    float roomTemperature = 0;
    if (Targets_configured(TARGET_SLOT_AC) && AcClient_roomTemperature(roomTemperature))
    {
        appendf(buffer, sizeof(buffer), used, "%s#8a97a8 room#  %.1f\xC2\xB0",
                used ? "\n" : "", roomTemperature);
    }
    Theme_setText(cornerTemps, buffer);

    // Bottom left: whatever sensor was pinned here.
    if (SensorBindings_isBound(SENSOR_SLOT_REST))
    {
        const String value = SensorBindings_value(SENSOR_SLOT_REST);
        snprintf(buffer, sizeof(buffer), "#8a97a8 %s#\n%s",
                 safe(SensorBindings_resource(SENSOR_SLOT_REST)).c_str(),
                 value.length() ? safe(value).c_str() : "--");
        Theme_setText(cornerSensor, buffer);
    }
    else
    {
        Theme_setText(cornerSensor, "");
    }

    // Bottom right: what is currently on.
    buffer[0] = '\0';
    used = 0;
    if (LightControl_state() == LIGHT_ON)
    {
        appendf(buffer, sizeof(buffer), used, "#ff9849 " LV_SYMBOL_POWER " Light#");
    }
    if (Rgb_on())
    {
        appendf(buffer, sizeof(buffer), used, "%s#ff9849 " LV_SYMBOL_TINT " RGB#", used ? "\n" : "");
    }
    Theme_setText(cornerState, buffer);
}
