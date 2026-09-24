#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Net.h"
#include "../App/Heap.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <Version.h>
#include <WiFi.h>

// The panel's own settings -- everything here is about this device, not about
// the network it watches.
//
// Left column: the things worth changing from the wall. Right column: what the
// panel currently is, plus the one destructive action.
//
// Changes are written straight to the persistent settings store as they are
// made. There is no Save button because there is nothing to lose by applying
// immediately: brightness and the idle timeout are visible the moment they move.

namespace
{
    lv_obj_t *page = nullptr;

    lv_obj_t *activeSlider = nullptr;
    lv_obj_t *activeValue = nullptr;
    lv_obj_t *restSlider = nullptr;
    lv_obj_t *restValue = nullptr;
    lv_obj_t *idleSlider = nullptr;
    lv_obj_t *idleValue = nullptr;

    lv_obj_t *infoBody = nullptr;
    lv_obj_t *brokerButton = nullptr;
    lv_obj_t *manifestButton = nullptr;
    lv_obj_t *rebootButton = nullptr;

    // Reboot takes two taps. The first arms it for a few seconds; after that it
    // quietly disarms, so a single stray tap cannot leave a live trigger behind
    // for whoever touches the panel next.
    bool rebootArmed = false;
    uint32_t rebootArmedAtMs = 0;
    const uint32_t RebootArmWindowMs = 4000;

    void setRebootLabel(const char *text)
    {
        lv_obj_t *label = lv_obj_get_child(rebootButton, 0);
        if (label)
        {
            lv_label_set_text(label, text);
        }
    }

    /// The idle slider is in steps rather than raw seconds: a 0-600 slider is
    /// impossible to land on a round number with a fingertip.
    const uint16_t IdleChoices[] = {0, 15, 30, 60, 120, 300, 600};
    const int IdleChoiceCount = sizeof(IdleChoices) / sizeof(IdleChoices[0]);

    int nearestIdleChoice(uint32_t seconds)
    {
        int best = 0;
        uint32_t bestGap = UINT32_MAX;
        for (int i = 0; i < IdleChoiceCount; ++i)
        {
            const uint32_t gap = seconds > IdleChoices[i] ? seconds - IdleChoices[i]
                                                          : IdleChoices[i] - seconds;
            if (gap < bestGap)
            {
                bestGap = gap;
                best = i;
            }
        }
        return best;
    }

    void setIdleLabel(int choice)
    {
        const uint16_t seconds = IdleChoices[choice];
        if (seconds == 0)
        {
            lv_label_set_text(idleValue, "never");
        }
        else if (seconds < 60)
        {
            lv_label_set_text_fmt(idleValue, "%ds", seconds);
        }
        else
        {
            lv_label_set_text_fmt(idleValue, "%dm", seconds / 60);
        }
    }

#pragma region "Events"

    void onActiveBacklight(lv_event_t *event)
    {
        const int value = lv_slider_get_value(activeSlider);
        lv_label_set_text_fmt(activeValue, "%d%%", (value * 100) / 255);
        // Applied live while dragging, so the slider is its own preview.
        set_backlight((uint8_t)value);
    }

    void onActiveBacklightDone(lv_event_t *event)
    {
        PersistentSettings.set("bl_active", String(lv_slider_get_value(activeSlider)));
        Ui_reloadSettings();
    }

    void onRestBacklight(lv_event_t *event)
    {
        const int value = lv_slider_get_value(restSlider);
        lv_label_set_text_fmt(restValue, "%d%%", (value * 100) / 255);
    }

    void onRestBacklightDone(lv_event_t *event)
    {
        PersistentSettings.set("bl_rest", String(lv_slider_get_value(restSlider)));
        Ui_reloadSettings();
    }

    void onIdle(lv_event_t *event)
    {
        setIdleLabel(lv_slider_get_value(idleSlider));
    }

    void onIdleDone(lv_event_t *event)
    {
        const int choice = lv_slider_get_value(idleSlider);
        PersistentSettings.set("rest_after", String(IdleChoices[choice]));
        Ui_reloadSettings();
    }

    void onRestNow(lv_event_t *event)
    {
        // Also the quickest way to preview the resting brightness just set.
        Ui_restNow();
    }

    void onBroker(lv_event_t *event)
    {
        // Swap between the local broker and the cloud one. Takes effect at once
        // -- the client reconnects -- and is not persisted, because which broker
        // is reachable is a property of where the panel is, not of the panel.
        //
        // Nothing switches it back on its own: the library has no automatic
        // broker failover any more, so the choice made here holds until it is
        // made again or the panel reboots.
        MQTT_change_to(!MQTT_isLocal());
        Ui_markDirty();
    }

    void onRequestManifests(lv_event_t *event)
    {
        Net_requestAllManifests();
    }

    void onReboot(lv_event_t *event)
    {
        if (!rebootArmed)
        {
            rebootArmed = true;
            rebootArmedAtMs = millis();
            setRebootLabel("Tap again to confirm");
            return;
        }
        ESP.restart();
    }

#pragma endregion

    /// A labelled slider row: caption and current value on one line, the slider
    /// under it, so the value stays readable at arm's length.
    lv_obj_t *sliderRow(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut,
                        int min, int max, lv_event_cb_t onChange, lv_event_cb_t onDone)
    {
        const lv_coord_t inner = UI_COL_W - 22;

        lv_obj_t *header = Theme_row(parent, inner, LV_SIZE_CONTENT);
        Theme_label(header, caption, &lv_font_montserrat_14, UI_COL_TEXT);
        *valueOut = Theme_label(header, "", &lv_font_montserrat_14, UI_COL_ACCENT);

        lv_obj_t *slider = lv_slider_create(parent);
        lv_obj_set_width(slider, inner);
        lv_slider_set_range(slider, min, max);
        lv_obj_set_style_bg_color(slider, UI_COL_SURFACE_ALT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, UI_COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, UI_COL_TEXT, LV_PART_KNOB);
        lv_obj_add_event_cb(slider, onChange, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(slider, onDone, LV_EVENT_RELEASED, nullptr);
        return slider;
    }
}

lv_obj_t *ScreenSettings_create(lv_obj_t *parent)
{
    page = lv_obj_create(parent);
    Theme_plainContainer(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, UI_GUTTER, LV_PART_MAIN);

    // Left: what can be changed.
    lv_obj_t *left = Theme_card(page, UI_COL_W, LV_PCT(100));
    Theme_cardHeader(left, "Display");
    lv_obj_set_style_pad_row(left, 4, LV_PART_MAIN);

    activeSlider = sliderRow(left, "Brightness", &activeValue, 8, 255,
                             onActiveBacklight, onActiveBacklightDone);
    restSlider = sliderRow(left, "While resting", &restValue, 0, 255,
                           onRestBacklight, onRestBacklightDone);
    idleSlider = sliderRow(left, "Rest after", &idleValue, 0, IdleChoiceCount - 1,
                           onIdle, onIdleDone);

    // Pushed to the bottom of the card, clear of the sliders, so a drag that
    // overshoots the last slider does not land on it.
    lv_obj_t *spacer = lv_obj_create(left);
    Theme_plainContainer(spacer);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *restNow = Theme_button(left, LV_SYMBOL_EYE_CLOSE "  Rest now", UI_COL_W - 22, 40);
    lv_obj_add_event_cb(restNow, onRestNow, LV_EVENT_CLICKED, nullptr);

    // Right: what the panel is, and the one thing that restarts it.
    lv_obj_t *right = Theme_card(page, UI_COL_W, LV_PCT(100));
    lv_obj_set_style_pad_row(right, 6, LV_PART_MAIN);
    // Seven lines of info plus two buttons is right at the 320px limit, and a
    // long SSID or device name can wrap a line. Scrolling beats clipping the
    // reboot button off the bottom.
    lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(right, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_AUTO);
    Theme_cardHeader(right, "This panel");

    infoBody = Theme_label(right, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
    lv_label_set_long_mode(infoBody, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(infoBody, true);
    lv_obj_set_width(infoBody, UI_COL_W - 22);
    lv_obj_set_style_text_line_space(infoBody, 1, LV_PART_MAIN);

    brokerButton = Theme_button(right, "", UI_COL_W - 22, 34);
    lv_obj_add_event_cb(brokerButton, onBroker, LV_EVENT_CLICKED, nullptr);

    manifestButton = Theme_button(right, LV_SYMBOL_DOWNLOAD "  Request manifests",
                                  UI_COL_W - 22, 34);
    lv_obj_add_event_cb(manifestButton, onRequestManifests, LV_EVENT_CLICKED, nullptr);

    rebootButton = Theme_button(right, LV_SYMBOL_REFRESH "  Reboot panel", UI_COL_W - 22, 38);
    lv_obj_set_style_text_color(rebootButton, UI_COL_DANGER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rebootButton, UI_COL_DANGER, LV_PART_MAIN);
    lv_obj_add_event_cb(rebootButton, onReboot, LV_EVENT_CLICKED, nullptr);

    // Seed the controls from stored config.
    const int active = constrain(PersistentSettings.get("bl_active", "200").toInt(), 8, 255);
    const int resting = constrain(PersistentSettings.get("bl_rest", "24").toInt(), 0, 255);
    const int idleChoice = nearestIdleChoice(PersistentSettings.get("rest_after", "60").toInt());

    lv_slider_set_value(activeSlider, active, LV_ANIM_OFF);
    lv_label_set_text_fmt(activeValue, "%d%%", (active * 100) / 255);
    lv_slider_set_value(restSlider, resting, LV_ANIM_OFF);
    lv_label_set_text_fmt(restValue, "%d%%", (resting * 100) / 255);
    lv_slider_set_value(idleSlider, idleChoice, LV_ANIM_OFF);
    setIdleLabel(idleChoice);

    return page;
}

void ScreenSettings_refresh()
{
    const uint32_t uptime = millis() / 1000;

    char text[320];
    snprintf(text, sizeof(text),
             "#8a97a8 name#  %s\n"
             "#8a97a8 firmware#  %s\n"
             "#8a97a8 ip#  %s\n"
             "#8a97a8 wifi#  %s\n"
             "#8a97a8 broker#  %s\n"
             "#8a97a8 uptime#  %lud %luh %02lum\n"
             "#8a97a8 heap#  %u free / %u largest",
             gDeviceIdentity.getDeviceName().c_str(),
             VERSION,
             WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "--",
             WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "disconnected",
             !MQTT_Connected() ? "offline" : (MQTT_isLocal() ? "local" : "cloud"),
             (unsigned long)(uptime / 86400),
             (unsigned long)((uptime % 86400) / 3600),
             (unsigned long)((uptime % 3600) / 60),
             (unsigned)Heap_free(),
             (unsigned)Heap_largest());
    lv_label_set_text(infoBody, text);

    lv_obj_t *label = lv_obj_get_child(brokerButton, 0);
    if (label)
    {
        lv_label_set_text(label, MQTT_isLocal() ? "Switch to cloud broker"
                                                : "Switch to local broker");
    }

    if (MQTT_Connected())
    {
        lv_obj_clear_state(manifestButton, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(manifestButton, LV_STATE_DISABLED);
    }

    // This page refreshes continuously while visible, and again on the first
    // tick after returning to it, so an expired arm is always cleared before
    // the button can be tapped again.
    if (rebootArmed && (millis() - rebootArmedAtMs) >= RebootArmWindowMs)
    {
        rebootArmed = false;
        setRebootLabel(LV_SYMBOL_REFRESH "  Reboot panel");
    }
}
