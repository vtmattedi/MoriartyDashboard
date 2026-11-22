#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"
#include "Pager.h"

#include "App/Net.h"
#include "App/Heap.h"
#include "App/Battery.h"
#include "App/Ota.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <Version.h>
#include "esp_ota_ops.h"

// The panel's own settings, in pages you swipe between -- each page is one
// subject and fills the screen, so nothing scrolls:
//
//   Display   brightness, the two timeouts, Rest now / Sleep now
//   Power     the cell, and what the three power stages are doing
//   Network   name, address, Wi-Fi and ESP-NOW
//   Update    firmware update from the PC
//   System    uptime, memory, reboot
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
    lv_obj_t *sleepSlider = nullptr;
    lv_obj_t *sleepValue = nullptr;

    Pager pager;

    lv_obj_t *batteryBig = nullptr;
    lv_obj_t *batteryBody = nullptr;
    lv_obj_t *powerBody = nullptr;
    lv_obj_t *networkBody = nullptr;
    lv_obj_t *firmwareBody = nullptr;
    lv_obj_t *systemBody = nullptr;
    lv_obj_t *transportButton = nullptr;
    lv_obj_t *manifestButton = nullptr;
    lv_obj_t *rebootButton = nullptr;
    lv_obj_t *otaBody = nullptr;
    lv_obj_t *otaButton = nullptr;

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

    /// The same idea for the second timeout, on a longer scale: this one is
    /// counted from the moment the resting clock appears.
    const uint16_t SleepChoices[] = {0, 30, 60, 120, 300, 600, 1800, 3600};
    const int SleepChoiceCount = sizeof(SleepChoices) / sizeof(SleepChoices[0]);

    int nearestChoice(const uint16_t *choices, int count, uint32_t seconds)
    {
        int best = 0;
        uint32_t bestGap = UINT32_MAX;
        for (int i = 0; i < count; ++i)
        {
            const uint32_t gap = seconds > choices[i] ? seconds - choices[i]
                                                      : choices[i] - seconds;
            if (gap < bestGap)
            {
                bestGap = gap;
                best = i;
            }
        }
        return best;
    }

    void setDurationLabel(lv_obj_t *label, uint16_t seconds)
    {
        if (seconds == 0)
        {
            lv_label_set_text(label, "never");
        }
        else if (seconds < 60)
        {
            lv_label_set_text_fmt(label, "%ds", seconds);
        }
        else if (seconds < 3600)
        {
            lv_label_set_text_fmt(label, "%dm", seconds / 60);
        }
        else
        {
            lv_label_set_text_fmt(label, "%dh", seconds / 3600);
        }
    }

    void setIdleLabel(int choice)
    {
        setDurationLabel(idleValue, IdleChoices[choice]);
    }

    void setSleepLabel(int choice)
    {
        setDurationLabel(sleepValue, SleepChoices[choice]);
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
        PersistentSettings.set("bl_active", std::to_string(lv_slider_get_value(activeSlider)));
        Ui_reloadSettings();
    }

    void onRestBacklight(lv_event_t *event)
    {
        const int value = lv_slider_get_value(restSlider);
        lv_label_set_text_fmt(restValue, "%d%%", (value * 100) / 255);
    }

    void onRestBacklightDone(lv_event_t *event)
    {
        PersistentSettings.set("bl_rest", std::to_string(lv_slider_get_value(restSlider)));
        Ui_reloadSettings();
    }

    void onIdle(lv_event_t *event)
    {
        setIdleLabel(lv_slider_get_value(idleSlider));
    }

    void onIdleDone(lv_event_t *event)
    {
        const int choice = lv_slider_get_value(idleSlider);
        PersistentSettings.set("rest_after", std::to_string(IdleChoices[choice]));
        Ui_reloadSettings();
    }

    void onSleep(lv_event_t *event)
    {
        setSleepLabel(lv_slider_get_value(sleepSlider));
    }

    void onSleepDone(lv_event_t *event)
    {
        const int choice = lv_slider_get_value(sleepSlider);
        PersistentSettings.set("sleep_after", std::to_string(SleepChoices[choice]));
        Ui_reloadSettings();
    }

    void onRestNow(lv_event_t *event)
    {
        // Also the quickest way to preview the resting brightness just set.
        Ui_restNow();
    }

    void onSleepNow(lv_event_t *event)
    {
        Ui_sleepNow();
    }

    void onRequestManifests(lv_event_t *event)
    {
        Net_requestAllManifests();
    }

    void onOta(lv_event_t *event)
    {
        if (Ota_active())
        {
            Ota_stop();
        }
        else
        {
            // The reason, if it refuses, is shown by the next refresh.
            Ota_start(nullptr);
        }
    }

    void onReboot(lv_event_t *event)
    {
        if (!rebootArmed)
        {
            rebootArmed = true;
            rebootArmedAtMs = millisNow();
            setRebootLabel("Tap again to confirm");
            return;
        }
        esp_restart();
    }

#pragma endregion

    /// A labelled slider row: caption and current value on one line, the slider
    /// under it, so the value stays readable at arm's length.
    lv_obj_t *sliderRow(lv_obj_t *parent, const char *caption, lv_obj_t **valueOut,
                        int min, int max, lv_event_cb_t onChange, lv_event_cb_t onDone)
    {
        const lv_coord_t inner = UI_CARD_INNER;

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
        // Dragged sideways: must not also turn the page.
        Pager_noSwipe(slider);
        return slider;
    }
}

namespace
{
    /// One page of the pager as a column of cards.
    lv_obj_t *columnPage(int index)
    {
        lv_obj_t *page = pager.page[index];
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
        lv_obj_set_style_pad_row(page, UI_GUTTER, LV_PART_MAIN);
        return page;
    }

    lv_obj_t *bodyLabel(lv_obj_t *card)
    {
        lv_obj_t *label = Theme_label(card, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_recolor(label, true);
        lv_obj_set_width(label, UI_CARD_INNER);
        lv_obj_set_style_text_line_space(label, 3, LV_PART_MAIN);
        return label;
    }

    void buildDisplayPage()
    {
        lv_obj_t *page = columnPage(0);
        lv_obj_t *card = Theme_card(page, UI_CARD_W, UI_PAGE_H_PAGER - 2 * UI_GUTTER);
        Theme_cardHeader(card, LV_SYMBOL_EYE_OPEN "  Display");
        lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);

        activeSlider = sliderRow(card, "Brightness", &activeValue, 8, 255,
                                 onActiveBacklight, onActiveBacklightDone);
        restSlider = sliderRow(card, "While resting", &restValue, 0, 255,
                               onRestBacklight, onRestBacklightDone);
        idleSlider = sliderRow(card, "Rest after", &idleValue, 0, IdleChoiceCount - 1,
                               onIdle, onIdleDone);
        sleepSlider = sliderRow(card, "Then sleep after", &sleepValue, 0, SleepChoiceCount - 1,
                                onSleep, onSleepDone);

        // A gap above the buttons, clear of the sliders, so a drag that
        // overshoots the last slider does not land on one.
        lv_obj_t *spacer = lv_obj_create(card);
        Theme_plainContainer(spacer);
        lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(spacer, 1, 8);

        const lv_coord_t half = (UI_CARD_INNER - 8) / 2;
        lv_obj_t *nowRow = Theme_row(card, UI_CARD_INNER, LV_SIZE_CONTENT);
        lv_obj_t *restNow = Theme_button(nowRow, LV_SYMBOL_EYE_CLOSE "  Rest now", half, 48);
        lv_obj_add_event_cb(restNow, onRestNow, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *sleepNow = Theme_button(nowRow, LV_SYMBOL_POWER "  Sleep now", half, 48);
        lv_obj_add_event_cb(sleepNow, onSleepNow, LV_EVENT_CLICKED, nullptr);

        // Seed the controls from stored config.
        const int active = clampTo(Text::toLong(PersistentSettings.get("bl_active", "200")), 8, 255);
        const int resting = clampTo(Text::toLong(PersistentSettings.get("bl_rest", "24")), 0, 255);
        const int idleChoice = nearestChoice(IdleChoices, IdleChoiceCount,
                                             Text::toLong(PersistentSettings.get("rest_after", "60")));
        const int sleepChoice = nearestChoice(SleepChoices, SleepChoiceCount,
                                              Text::toLong(PersistentSettings.get("sleep_after", "300")));
        lv_slider_set_value(activeSlider, active, LV_ANIM_OFF);
        lv_label_set_text_fmt(activeValue, "%d%%", (active * 100) / 255);
        lv_slider_set_value(restSlider, resting, LV_ANIM_OFF);
        lv_label_set_text_fmt(restValue, "%d%%", (resting * 100) / 255);
        lv_slider_set_value(idleSlider, idleChoice, LV_ANIM_OFF);
        setIdleLabel(idleChoice);
        lv_slider_set_value(sleepSlider, sleepChoice, LV_ANIM_OFF);
        setSleepLabel(sleepChoice);
    }

    void buildPowerPage()
    {
        lv_obj_t *page = columnPage(1);
        lv_obj_t *cell = Theme_card(page, UI_CARD_W, 190);
        Theme_cardHeader(cell, LV_SYMBOL_BATTERY_3 "  Battery");
        batteryBig = Theme_label(cell, "--", &lv_font_montserrat_48, UI_COL_TEXT);
        batteryBody = bodyLabel(cell);

        lv_obj_t *stages = Theme_card(page, UI_CARD_W, UI_PAGE_H_PAGER - 3 * UI_GUTTER - 190);
        Theme_cardHeader(stages, LV_SYMBOL_POWER "  Power stages");
        powerBody = bodyLabel(stages);
    }

    void buildNetworkPage()
    {
        lv_obj_t *page = columnPage(2);
        lv_obj_t *card = Theme_card(page, UI_CARD_W, UI_PAGE_H_PAGER - 2 * UI_GUTTER);
        lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
        Theme_cardHeader(card, LV_SYMBOL_WIFI "  Network");
        networkBody = bodyLabel(card);

        transportButton = Theme_button(card, "ESP-NOW transport", UI_CARD_INNER, 44);
        lv_obj_add_state(transportButton, LV_STATE_DISABLED);
        manifestButton = Theme_button(card, LV_SYMBOL_DOWNLOAD "  Request manifests", UI_CARD_INNER, 44);
        lv_obj_add_event_cb(manifestButton, onRequestManifests, LV_EVENT_CLICKED, nullptr);
    }

    void buildUpdatePage()
    {
        lv_obj_t *page = columnPage(3);
        lv_obj_t *update = Theme_card(page, UI_CARD_W, 210);
        lv_obj_set_style_pad_row(update, 8, LV_PART_MAIN);
        Theme_cardHeader(update, LV_SYMBOL_UPLOAD "  Firmware update");
        otaBody = bodyLabel(update);
        otaButton = Theme_button(update, LV_SYMBOL_UPLOAD "  Update from PC", UI_CARD_INNER, 48);
        lv_obj_add_event_cb(otaButton, onOta, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *build = Theme_card(page, UI_CARD_W, UI_PAGE_H_PAGER - 3 * UI_GUTTER - 210);
        Theme_cardHeader(build, LV_SYMBOL_FILE "  This build");
        firmwareBody = bodyLabel(build);
    }

    void buildSystemPage()
    {
        lv_obj_t *page = columnPage(4);
        lv_obj_t *card = Theme_card(page, UI_CARD_W, UI_PAGE_H_PAGER - 2 * UI_GUTTER);
        lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
        Theme_cardHeader(card, LV_SYMBOL_SETTINGS "  System");
        systemBody = bodyLabel(card);

        rebootButton = Theme_button(card, LV_SYMBOL_REFRESH "  Reboot panel", UI_CARD_INNER, 52);
        lv_obj_set_style_text_color(rebootButton, UI_COL_DANGER, LV_PART_MAIN);
        lv_obj_set_style_border_color(rebootButton, UI_COL_DANGER, LV_PART_MAIN);
        lv_obj_add_event_cb(rebootButton, onReboot, LV_EVENT_CLICKED, nullptr);
    }
}

lv_obj_t *ScreenSettings_create(lv_obj_t *parent)
{
    Pager_create(pager, parent, SETTINGS_PAGE_COUNT);
    page = pager.root;
    buildDisplayPage();
    buildPowerPage();
    buildNetworkPage();
    buildUpdatePage();
    buildSystemPage();
    return page;
}

void ScreenSettings_home()
{
    Pager_go(pager, 0);
}

void ScreenSettings_open(int index)
{
    Pager_go(pager, index);
    Ui_show(UI_PAGE_SETTINGS);
}

void ScreenSettings_refresh()
{
    switch (pager.current)
    {
    case SETTINGS_PAGE_POWER:
    {
        char big[16];
        char body[200];
        if (Battery_present())
        {
            snprintf(big, sizeof(big), "%u%%", (unsigned)Battery_percent());
            snprintf(body, sizeof(body),
                     "#8a97a8 voltage#  %u.%02u V\n"
                     "#8a97a8 state#  %s\n"
                     "The percentage is an estimate from the resting voltage; it sags under load and cannot "
                     "tell charging from full.",
                     (unsigned)(Battery_millivolts() / 1000), (unsigned)((Battery_millivolts() % 1000) / 10),
                     Battery_low() ? "#f2566a low#" : "ok");
        }
        else
        {
            snprintf(big, sizeof(big), "--");
            snprintf(body, sizeof(body), "No cell detected: the panel is on USB with nothing fitted.");
        }
        lv_obj_set_style_text_color(batteryBig, Battery_present() && Battery_low() ? UI_COL_DANGER : UI_COL_TEXT,
                                    LV_PART_MAIN);
        Theme_setText(batteryBig, big);
        Theme_setText(batteryBody, body);

        Theme_setText(powerBody,
                      "#8a97a8 1 active#  full brightness, everything live\n"
                      "#8a97a8 2 resting#  the clock, dimmed; the network stays up\n"
                      "#8a97a8 3 asleep#  radio and screen off, until touched\n\n"
                      "Set the two timeouts on the Display page.");
        break;
    }
    case SETTINGS_PAGE_NETWORK:
    {
        const NightMare::WiFiInfo wifi = WiFi_info();
        const NightMare::ConnectionState connection = NightMare::GetConnectionState();
        const char *connectionText =
            connection == NightMare::ConnectionState::CONNECTED ? "connected" :
            connection == NightMare::ConnectionState::DISCOVERING ? "discovering" :
            connection == NightMare::ConnectionState::CONNECTING ? "connecting" :
            connection == NightMare::ConnectionState::ERROR ? "error" : "stopped";
        char text[300];
        snprintf(text, sizeof(text),
                 "#8a97a8 name#  %s\n"
                 "#8a97a8 ip#  %s\n"
                 "#8a97a8 wifi#  %s\n"
                 "#8a97a8 esp-now#  %s",
                 gDeviceIdentity.getDisplayName().c_str(),
                 wifi.state == NightMare::WiFiState::CONNECTED ? wifi.ip.c_str() : "--",
                 wifi.state == NightMare::WiFiState::CONNECTED ? wifi.ssid.c_str() : "disconnected",
                 connectionText);
        Theme_setText(networkBody, text);
        if (connection == NightMare::ConnectionState::CONNECTED)
        {
            lv_obj_clear_state(manifestButton, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(manifestButton, LV_STATE_DISABLED);
        }
        break;
    }
    case SETTINGS_PAGE_UPDATE:
    {
        std::string status = Ota_message();
        if (status.empty())
        {
            status = "Open update mode, then on the PC run:\ndashlink push-fw --target dashboard --host <ip>";
        }
        Theme_setText(otaBody, status.c_str());

        const OtaState state = Ota_state();
        const bool busy = state == OtaState::Receiving || state == OtaState::Verifying || state == OtaState::Done;
        lv_obj_t *label = lv_obj_get_child(otaButton, 0);
        if (label)
        {
            Theme_setText(label, busy ? "Updating..."
                                      : Ota_active() ? LV_SYMBOL_CLOSE "  Close update mode"
                                                     : LV_SYMBOL_UPLOAD "  Update from PC");
        }
        // A transfer in flight is not something a tap should be able to drop.
        if (busy)
        {
            lv_obj_add_state(otaButton, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_clear_state(otaButton, LV_STATE_DISABLED);
        }

        const esp_partition_t *running = esp_ota_get_running_partition();
        char text[200];
        snprintf(text, sizeof(text),
                 "#8a97a8 firmware#  %s\n"
                 "#8a97a8 built#  %s\n"
                 "#8a97a8 running from#  %s",
                 VERSION, BUILD_TIMESTAMP, running ? running->label : "?");
        Theme_setText(firmwareBody, text);
        break;
    }
    case SETTINGS_PAGE_SYSTEM:
    {
        const uint32_t uptime = millisNow() / 1000;
        char text[200];
        snprintf(text, sizeof(text),
                 "#8a97a8 uptime#  %lud %luh %02lum\n"
                 "#8a97a8 heap#  %u free\n"
                 "#8a97a8 largest block#  %u",
                 (unsigned long)(uptime / 86400), (unsigned long)((uptime % 86400) / 3600),
                 (unsigned long)((uptime % 3600) / 60), (unsigned)Heap_free(), (unsigned)Heap_largest());
        Theme_setText(systemBody, text);

        // This page refreshes continuously while visible, so an expired arm is
        // always cleared before the button can be tapped again.
        if (rebootArmed && (millisNow() - rebootArmedAtMs) >= RebootArmWindowMs)
        {
            rebootArmed = false;
            setRebootLabel(LV_SYMBOL_REFRESH "  Reboot panel");
        }
        break;
    }
    default:
        break;
    }
}
