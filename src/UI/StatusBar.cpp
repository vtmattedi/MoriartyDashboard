#include "Screens.h"
#include "Theme.h"

#include "../App/Net.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <WiFi.h>

namespace
{
    lv_obj_t *clockLabel = nullptr;
    lv_obj_t *nameLabel = nullptr;
    lv_obj_t *wifiIcon = nullptr;
    lv_obj_t *mqttIcon = nullptr;
}

lv_obj_t *StatusBar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    Theme_plainContainer(bar);
    lv_obj_set_size(bar, SCREEN_WIDTH, UI_STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COL_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, UI_GUTTER, LV_PART_MAIN);

    clockLabel = Theme_label(bar, "--:--", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(clockLabel, LV_ALIGN_LEFT_MID, 0, 0);

    nameLabel = Theme_label(bar, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_align(nameLabel, LV_ALIGN_CENTER, 0, 0);

    mqttIcon = Theme_label(bar, LV_SYMBOL_LOOP, &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_align(mqttIcon, LV_ALIGN_RIGHT_MID, 0, 0);

    wifiIcon = Theme_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_align(wifiIcon, LV_ALIGN_RIGHT_MID, -24, 0);

    return bar;
}

void StatusBar_refresh()
{
    // Before the first time sync the clock would read 00:0x and look like a
    // real time, so say plainly that it is not set yet.
    // Runs four times a second on every page, so text is only rewritten when it
    // actually changes -- see Theme_setText.
    if (!NightMare::Time::valid())
    {
        Theme_setText(clockLabel, "--:--");
    }
    else
    {
        // timeString() formats in the process timezone. The library's hour() and
        // minute() are deliberately UTC, so they are the wrong tool for a clock
        // someone reads off a wall.
        Theme_setText(clockLabel, NightMare::Time::timeString().c_str());
    }

    Theme_setText(nameLabel, gDeviceIdentity.getDeviceName().c_str());

    const bool wifiUp = WiFi.status() == WL_CONNECTED;
    lv_obj_set_style_text_color(wifiIcon, wifiUp ? UI_COL_OK : UI_COL_DANGER, LV_PART_MAIN);

    const bool mqttUp = MQTT_Connected();
    lv_obj_set_style_text_color(mqttIcon, mqttUp ? UI_COL_OK : UI_COL_DANGER, LV_PART_MAIN);
}
