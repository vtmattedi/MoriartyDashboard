#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"

#include "App/Net.h"
#include "App/Battery.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <stdio.h>

namespace
{
    lv_obj_t *clockLabel = nullptr;
    lv_obj_t *nameLabel = nullptr;
    lv_obj_t *backButton = nullptr;
    bool backShown = false;
    lv_obj_t *wifiIcon = nullptr;
    lv_obj_t *networkIcon = nullptr;
    lv_obj_t *batteryLabel = nullptr;

    /// LVGL ships five battery glyphs, empty to full.
    const char *batterySymbol(uint8_t percent)
    {
        if (percent >= 88)
        {
            return LV_SYMBOL_BATTERY_FULL;
        }
        if (percent >= 63)
        {
            return LV_SYMBOL_BATTERY_3;
        }
        if (percent >= 38)
        {
            return LV_SYMBOL_BATTERY_2;
        }
        if (percent >= 13)
        {
            return LV_SYMBOL_BATTERY_1;
        }
        return LV_SYMBOL_BATTERY_EMPTY;
    }
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

    // Left: a back arrow (hidden on the menu, which has nowhere to go back to)
    // and the page's title. The title gives way first when the right-hand
    // cluster needs the room.
    backButton = lv_btn_create(bar);
    lv_obj_remove_style_all(backButton);
    lv_obj_set_size(backButton, 40, UI_STATUSBAR_H);
    lv_obj_align(backButton, LV_ALIGN_LEFT_MID, -UI_GUTTER, 0);
    lv_obj_set_style_text_color(backButton, UI_COL_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(backButton, UI_COL_TEXT, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(backButton, [](lv_event_t *) { Ui_back(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *arrow = lv_label_create(backButton);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_center(arrow);
    lv_obj_add_flag(backButton, LV_OBJ_FLAG_HIDDEN);

    nameLabel = Theme_label(bar, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nameLabel, 100);
    lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 0, 0);

    // The clock sits in the right-hand cluster, left of the battery.
    clockLabel = Theme_label(bar, "--:--", &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(clockLabel, LV_ALIGN_RIGHT_MID, -112, 0);

    networkIcon = Theme_label(bar, LV_SYMBOL_LOOP, &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_align(networkIcon, LV_ALIGN_RIGHT_MID, 0, 0);

    wifiIcon = Theme_label(bar, LV_SYMBOL_WIFI, &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_align(wifiIcon, LV_ALIGN_RIGHT_MID, -24, 0);

    // Anchored by its right edge, so it grows leftwards as "9%" becomes "100%"
    // and the icons beside it stay put.
    batteryLabel = Theme_label(bar, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
    lv_obj_align(batteryLabel, LV_ALIGN_RIGHT_MID, -50, 0);

    return bar;
}

void StatusBar_setTitle(const char *title, bool back)
{
    Theme_setText(nameLabel, title);
    if (back != backShown)
    {
        backShown = back;
        if (back)
        {
            lv_obj_clear_flag(backButton, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 28, 0);
        }
        else
        {
            lv_obj_add_flag(backButton, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }
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

    const bool wifiUp = WiFi_state() == NightMare::WiFiState::CONNECTED;
    lv_obj_set_style_text_color(wifiIcon, wifiUp ? UI_COL_OK : UI_COL_DANGER, LV_PART_MAIN);

    const bool networkUp =
        NightMare::GetConnectionState() == NightMare::ConnectionState::CONNECTED;
    lv_obj_set_style_text_color(networkIcon, networkUp ? UI_COL_OK : UI_COL_DANGER,
                                LV_PART_MAIN);

    // Nothing at all without a cell: on USB with no battery fitted there is no
    // level to show, and a red empty battery would be a false alarm.
    if (!Battery_present())
    {
        Theme_setText(batteryLabel, "");
    }
    else
    {
        const uint8_t percent = Battery_percent();
        char text[24];
        snprintf(text, sizeof(text), "%u%% %s", (unsigned)percent, batterySymbol(percent));
        Theme_setText(batteryLabel, text);
        lv_obj_set_style_text_color(batteryLabel, Battery_low() ? UI_COL_DANGER : UI_COL_TEXT_DIM,
                                    LV_PART_MAIN);
    }
}
