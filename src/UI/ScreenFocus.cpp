#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Net.h"
#include "../App/Targets.h"
#include "../App/Registry.h"
#include "../App/Forecast.h"
#include "../App/SensorBindings.h"
#include "../App/AcClient.h"
#include "../App/LightControl.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <stdio.h>

// The focus screen: everything this panel exists to control.
//
// Two columns, which is what the landscape width is for. The AC owns the left
// one -- it is the most important control, so it never scrolls out of reach and
// carries the largest type on the page. The light, the RGB light and the weather
// share the right column, which scrolls.
//
// The AC and light cards are themselves buttons: tapping the card toggles the
// thing it represents, and its colour is the state. Anything finer sits on a
// nested button, which LVGL does not bubble, so pressing it does not also toggle.
//
// Nothing here talks to MQTT. Every value comes out of a bound NightMare
// resource (App/AcClient.h, App/LightControl.h) and every command goes back
// through one, so a tap is a `/set` or an `/invoke` and the answer arrives as
// new owner state on the next refresh.
//
// Values are formatted with snprintf rather than lv_label_set_text_fmt because
// LVGL's own printf only handles %f when LV_SPRINTF_USE_FLOAT is set, and most
// of what is shown here is a temperature.

namespace
{
    /// Width available inside a column card, after its padding.
    const lv_coord_t CardInner = UI_COL_W - 22;

    lv_obj_t *page = nullptr;
    lv_obj_t *acCard = nullptr;
    lv_obj_t *sideColumn = nullptr;

    // Suppresses the value-changed events LVGL fires while a refresh is moving a
    // switch or slider to match the device. Without it, every incoming state
    // update would be echoed straight back out as a command.
    bool applyingRemoteState = false;

    void formatTemp(char *out, size_t size, double value, const char *suffix)
    {
        // One decimal only when there is one: "24" reads better than "24.0".
        const double absolute = value < 0 ? -value : value;
        if (absolute - (long)absolute < 0.05)
        {
            snprintf(out, size, "%ld%s", (long)(value + (value < 0 ? -0.001 : 0.001)), suffix);
        }
        else
        {
            snprintf(out, size, "%.1f%s", value, suffix);
        }
    }

    void setButtonText(lv_obj_t *button, const char *text)
    {
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label)
        {
            Theme_setText(label, text);
        }
    }

    /// A toggle-style button: lit in `colour` when on, plain when off.
    void setToggleLit(lv_obj_t *button, bool lit, lv_color_t colour)
    {
        lv_obj_set_style_text_color(button, lit ? colour : UI_COL_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_border_color(button, lit ? colour : UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, lit ? lv_color_mix(colour, UI_COL_SURFACE_ALT, LV_OPA_30)
                                              : UI_COL_SURFACE_ALT,
                                  LV_PART_MAIN);
    }

    enum Liveness
    {
        CARD_LIVE = 0,
        CARD_OFFLINE, // bound, but the device is not answering
        CARD_UNBOUND  // nothing chosen for this role
    };

    /// Whether there is anything on the other end to talk to. A device the panel
    /// has never heard from at all counts as offline: every NightMare device
    /// publishes a retained status on connect, so silence is real.
    Liveness livenessOf(TargetSlot slot)
    {
        if (!Targets_configured(slot))
        {
            return CARD_UNBOUND;
        }
        const DeviceEntry *entry = Registry_findDevice(Targets_device(slot));
        return (entry && entry->online) ? CARD_LIVE : CARD_OFFLINE;
    }

    /// Disabled controls stay on screen, greyed and inert, rather than being
    /// replaced by a notice: the page keeps its shape and what the panel would
    /// let you do stays visible.
    ///
    /// Done with explicit colours rather than style opacity on purpose. LVGL
    /// renders any object with LV_STYLE_OPA below 255 into an intermediate layer,
    /// asking for up to LV_LAYER_SIMPLE_BUF_SIZE (24KB) on every redraw -- which
    /// this board cannot spare for a greyed-out card.
    void applyButton(lv_obj_t *button, bool enabled, bool lit, lv_color_t colour);

    /// A whole card acting as a switch: tinted and outlined in `colour` when on.
    void setCardLit(lv_obj_t *card, bool lit, lv_color_t colour)
    {
        lv_obj_set_style_bg_color(card, lit ? lv_color_mix(colour, UI_COL_SURFACE, LV_OPA_20)
                                            : UI_COL_SURFACE,
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lit ? colour : UI_COL_BORDER, LV_PART_MAIN);
    }

    void applyButton(lv_obj_t *button, bool enabled, bool lit, lv_color_t colour)
    {
        if (!enabled)
        {
            lv_obj_add_state(button, LV_STATE_DISABLED);
            lv_obj_set_style_text_color(button, UI_COL_TEXT_FAINT, LV_PART_MAIN);
            lv_obj_set_style_border_color(button, UI_COL_BORDER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(button, UI_COL_SURFACE, LV_PART_MAIN);
            return;
        }
        lv_obj_clear_state(button, LV_STATE_DISABLED);
        setToggleLit(button, lit, colour);
    }

    /// Switches and sliders come from the LVGL theme, which dims its own disabled
    /// state, so these only need the state flag.
    void setWidgetEnabled(lv_obj_t *widget, bool enabled)
    {
        if (enabled)
        {
            lv_obj_clear_state(widget, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(widget, LV_STATE_DISABLED);
        }
    }

    lv_obj_t *makeCardButton(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
    {
        lv_obj_t *card = Theme_card(parent, width, height);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        return card;
    }

    lv_obj_t *unboundNotice(lv_obj_t *card, const char *text)
    {
        lv_obj_t *label = Theme_label(card, text, &lv_font_montserrat_14, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, CardInner);
        return label;
    }

    /// Shared by the compact card and the expanded panel.
    void applyAcChip(lv_obj_t *chip)
    {
        if (!Targets_configured(TARGET_SLOT_AC))
        {
            Theme_chipSet(chip, "unbound", UI_COL_TEXT_FAINT);
            return;
        }
        if (AcClient_isStale())
        {
            Theme_chipSet(chip, "no data", UI_COL_TEXT_FAINT);
            return;
        }
        switch (AcClient_state())
        {
        case AC_ON:
            Theme_chipSet(chip, "on", UI_COL_COOL);
            break;
        case AC_ON_TARGET:
            Theme_chipSet(chip, "auto - cooling", UI_COL_COOL);
            break;
        case AC_OFF_TARGET:
            Theme_chipSet(chip, "auto - idle", UI_COL_OK);
            break;
        case AC_OFF:
            Theme_chipSet(chip, "off", UI_COL_TEXT_DIM);
            break;
        case AC_OFF_DOOR_OPEN:
        case AC_OFF_TARGET_DOOR_OPEN:
            Theme_chipSet(chip, "paused - door", UI_COL_WARN);
            break;
        default:
            Theme_chipSet(chip, "unknown", UI_COL_TEXT_FAINT);
            break;
        }
    }

    /// The big number: the room target in auto mode, otherwise the unit's own
    /// setpoint. Returns the caption that says which it is.
    const char *formatAcSetpoint(char *out, size_t size)
    {
        if (AcClient_targetEnabled())
        {
            formatTemp(out, size, AcClient_target(), "\xC2\xB0");
            return "room target";
        }
        const int setpoint = AcClient_unitTemperature();
        snprintf(out, size, "%d\xC2\xB0", setpoint);
        // Power and setpoint are separate resources now, so "off" is what the
        // unit says about itself rather than a negative number standing in for it.
        return AcClient_powerOn() ? "unit setpoint" : "unit off";
    }

#pragma region "AC - shared actions"

    void onAcStep(lv_event_t *event)
    {
        // LVGL still delivers clicks to a button in LV_STATE_DISABLED, so every
        // handler checks for itself rather than trusting the greyed-out look.
        if (livenessOf(TARGET_SLOT_AC) != CARD_LIVE)
        {
            return;
        }
        const int direction = (int)(intptr_t)lv_event_get_user_data(event);

        // The two modes tune different things: with room-temperature control on
        // the step moves the target in half degrees, otherwise it moves the
        // unit's own setpoint in whole ones.
        if (AcClient_targetEnabled())
        {
            AcClient_stepTarget(direction * 0.5f);
        }
        else
        {
            AcClient_stepUnitTemperature(direction);
        }
        Ui_markDirty();
    }

    void onAcCard(lv_event_t *event)
    {
        switch (livenessOf(TARGET_SLOT_AC))
        {
        case CARD_UNBOUND:
            // Nothing to toggle; take them to where a device gets chosen.
            Ui_show(UI_PAGE_DEVICES);
            return;
        case CARD_OFFLINE:
            // The card is drawn disabled; a command would go nowhere.
            return;
        default:
            break;
        }
        AcClient_togglePower();
        Ui_markDirty();
    }

    void onAcAuto(lv_event_t *event)
    {
        if (livenessOf(TARGET_SLOT_AC) == CARD_LIVE)
        {
            AcClient_toggleTarget();
            Ui_markDirty();
        }
    }

#pragma endregion

#pragma region "AC - sleep timer"

    lv_obj_t *sleepModal = nullptr;
    lv_obj_t *sleepCurrent = nullptr;

    /// Minutes; 0 cancels.
    const uint16_t SleepChoices[] = {0, 30, 60, 120, 240};
    const char *const SleepLabels[] = {"Off", "30 min", "1 hour", "2 hours", "4 hours"};
    const int SleepChoiceCount = sizeof(SleepChoices) / sizeof(SleepChoices[0]);

    void onSleepChoice(lv_event_t *event)
    {
        const int index = (int)(intptr_t)lv_event_get_user_data(event);
        if (index >= 0 && index < SleepChoiceCount)
        {
            // The controller owns the timer and counts it down itself; the
            // deadline it publishes on `ac_sleep_deadline` is what the card
            // shows, so there is nothing to remember here.
            AcClient_setSleepMinutes(SleepChoices[index]);
        }
        lv_obj_add_flag(sleepModal, LV_OBJ_FLAG_HIDDEN);
        Ui_markDirty();
    }

    void buildSleepModal()
    {
        sleepModal = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(sleepModal);
        lv_obj_set_size(sleepModal, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_set_style_bg_color(sleepModal, UI_COL_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sleepModal, LV_OPA_90, LV_PART_MAIN);
        lv_obj_clear_flag(sleepModal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sleepModal, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *panel = Theme_card(sleepModal, 380, LV_SIZE_CONTENT);
        lv_obj_center(panel);
        lv_obj_set_style_pad_row(panel, 6, LV_PART_MAIN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        Theme_label(panel, "Sleep timer", &lv_font_montserrat_16, UI_COL_TEXT);
        lv_obj_t *note = Theme_label(panel,
                                     "The unit switches itself off; the panel need not be awake.",
                                     &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, 350);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        sleepCurrent = Theme_label(panel, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);

        // Two rows of choices: five buttons do not fit across 350px.
        lv_obj_t *grid = lv_obj_create(panel);
        Theme_plainContainer(grid);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(grid, 350, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(grid, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(grid, 6, LV_PART_MAIN);

        for (int i = 0; i < SleepChoiceCount; ++i)
        {
            lv_obj_t *button = Theme_button(grid, SleepLabels[i], 110, 38);
            lv_obj_add_event_cb(button, onSleepChoice, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        lv_obj_t *cancel = Theme_button(panel, "Cancel", 350, 36);
        lv_obj_add_event_cb(cancel, onSleepChoice, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    }

    /// The epoch second the unit is due to switch off, or 0 when there is
    /// nothing to show. Unlike the mock this replaces, it is the controller's
    /// own timer -- the panel neither keeps it nor counts it down.
    uint32_t sleepDeadline()
    {
        return Targets_configured(TARGET_SLOT_AC) ? AcClient_sleepDeadline() : 0;
    }

    /// Seconds left on the sleep timer, or 0 when there is none, the clock is
    /// not set, or it has already passed.
    uint32_t sleepRemaining()
    {
        const uint32_t deadline = sleepDeadline();
        if (!deadline || !NightMare::Time::valid())
        {
            return 0;
        }
        const uint32_t current = (uint32_t)NightMare::Time::now();
        return deadline > current ? deadline - current : 0;
    }

    void onSleepOpen(lv_event_t *event)
    {
        if (livenessOf(TARGET_SLOT_AC) != CARD_LIVE)
        {
            return;
        }

        const uint32_t remaining = sleepRemaining();
        if (remaining)
        {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "Now: %luh %02lum left",
                     (unsigned long)(remaining / 3600), (unsigned long)((remaining % 3600) / 60));
            Theme_setText(sleepCurrent, buffer);
        }
        else
        {
            Theme_setText(sleepCurrent, "Now: no sleep timer");
        }

        lv_obj_clear_flag(sleepModal, LV_OBJ_FLAG_HIDDEN);
    }

#pragma endregion

#pragma region "AC - expanded panel"

    lv_obj_t *acPanel = nullptr;
    lv_obj_t *panelChip = nullptr;
    lv_obj_t *panelMode = nullptr;
    lv_obj_t *panelBig = nullptr;
    lv_obj_t *panelRoom = nullptr;
    lv_obj_t *syncTempLabel = nullptr;
    lv_obj_t *syncOnButton = nullptr;
    lv_obj_t *syncOffButton = nullptr;
    lv_obj_t *syncButton = nullptr;

    // What the user says the physical unit is showing. Seeded from what the
    // controller believes each time the panel opens, since that is usually right
    // and only needs nudging.
    int syncTemp = 24;
    bool syncOn = true;
    uint32_t syncedAtMs = 0;
    const uint32_t SyncedFeedbackMs = 2500;

    void expandAc()
    {
        const int believed = AcClient_unitTemperature();
        syncTemp = constrain(believed > 0 ? believed : 24, AC_UNIT_TEMP_MIN, AC_UNIT_TEMP_MAX);
        syncOn = AcClient_powerOn();
        syncedAtMs = 0;

        lv_obj_add_flag(acCard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sideColumn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(acPanel, LV_OBJ_FLAG_HIDDEN);
        Ui_markDirty();
    }

    void onExpand(lv_event_t *event)
    {
        if (livenessOf(TARGET_SLOT_AC) == CARD_LIVE)
        {
            expandAc();
        }
    }

    void onCollapse(lv_event_t *event)
    {
        ScreenFocus_collapse();
    }

    void onSyncTempStep(lv_event_t *event)
    {
        const int direction = (int)(intptr_t)lv_event_get_user_data(event);
        syncTemp = constrain(syncTemp + direction, AC_UNIT_TEMP_MIN, AC_UNIT_TEMP_MAX);
        syncedAtMs = 0;
        Ui_markDirty();
    }

    void onSyncPower(lv_event_t *event)
    {
        syncOn = (bool)(intptr_t)lv_event_get_user_data(event);
        syncedAtMs = 0;
        Ui_markDirty();
    }

    void onSync(lv_event_t *event)
    {
        if (livenessOf(TARGET_SLOT_AC) != CARD_LIVE)
        {
            return;
        }
        // Not a command to the unit: this tells the controller what the unit is
        // already doing, after it was changed behind the controller's back
        // (usually with the unit's own IR remote). No confirmation for the same
        // reason -- it corrects a belief, it does not change the room.
        AcClient_manualSync(syncTemp, syncOn);
        syncedAtMs = millis();
        Ui_markDirty();
    }

    lv_obj_t *stepButton(lv_obj_t *parent, const char *symbol, lv_coord_t w, lv_coord_t h,
                         lv_event_cb_t callback, int direction)
    {
        lv_obj_t *button = Theme_button(parent, symbol, w, h);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *)(intptr_t)direction);
        return button;
    }

    void buildAcPanel(lv_obj_t *parent)
    {
        // Floating over the page's content area rather than part of its row
        // layout, so opening it is a hide/show and nothing reflows.
        acPanel = lv_obj_create(parent);
        Theme_plainContainer(acPanel);
        lv_obj_add_flag(acPanel, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_pos(acPanel, 0, 0);
        lv_obj_set_size(acPanel, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(acPanel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(acPanel, UI_GUTTER, LV_PART_MAIN);
        lv_obj_add_flag(acPanel, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *header = Theme_row(acPanel, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_t *back = Theme_button(header, LV_SYMBOL_LEFT, 44, 32);
        lv_obj_add_event_cb(back, onCollapse, LV_EVENT_CLICKED, nullptr);
        Theme_label(header, "Air Conditioning", &lv_font_montserrat_16, UI_COL_TEXT);
        panelChip = Theme_chip(header);

        // An explicit height rather than flex_grow: the cards inside are
        // LV_PCT(100) tall, and a percentage of a height the layout has not
        // resolved yet is circular -- LVGL settles that unpredictably.
        const lv_coord_t headerHeight = 32;
        const lv_coord_t bodyHeight = UI_CONTENT_H - (2 * UI_GUTTER) - headerHeight - UI_GUTTER;
        lv_obj_t *body = Theme_row(acPanel, LV_PCT(100), bodyHeight);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        // Left: the setpoint, adjusted.
        lv_obj_t *left = Theme_card(body, UI_COL_W, LV_PCT(100));
        lv_obj_set_style_pad_row(left, 5, LV_PART_MAIN);
        lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        panelMode = Theme_label(left, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        panelBig = Theme_label(left, "--", &lv_font_montserrat_48, UI_COL_TEXT);

        lv_obj_t *steps = Theme_row(left, CardInner, LV_SIZE_CONTENT);
        stepButton(steps, LV_SYMBOL_MINUS, (CardInner - 8) / 2, 46, onAcStep, -1);
        stepButton(steps, LV_SYMBOL_PLUS, (CardInner - 8) / 2, 46, onAcStep, 1);

        panelRoom = Theme_label(left, "", &lv_font_montserrat_16, UI_COL_TEXT_DIM);

        // Right: tell the controller what the unit is really doing. Scrollable
        // because the explanation wraps to two lines on a narrow column.
        lv_obj_t *right = Theme_card(body, UI_COL_W, LV_PCT(100));
        lv_obj_set_style_pad_row(right, 5, LV_PART_MAIN);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(right, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_AUTO);

        Theme_label(right, "Sync with unit", &lv_font_montserrat_16, UI_COL_TEXT);
        lv_obj_t *hint = Theme_label(right, "Set what the unit's display shows, then Sync.",
                                     &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, CardInner);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        lv_obj_t *tempRow = Theme_row(right, CardInner, LV_SIZE_CONTENT);
        stepButton(tempRow, LV_SYMBOL_MINUS, 52, 40, onSyncTempStep, -1);
        syncTempLabel = Theme_label(tempRow, "--", &lv_font_montserrat_28, UI_COL_TEXT);
        stepButton(tempRow, LV_SYMBOL_PLUS, 52, 40, onSyncTempStep, 1);

        lv_obj_t *powerRow = Theme_row(right, CardInner, LV_SIZE_CONTENT);
        syncOnButton = Theme_button(powerRow, "On", (CardInner - 8) / 2, 34);
        lv_obj_add_event_cb(syncOnButton, onSyncPower, LV_EVENT_CLICKED, (void *)(intptr_t) true);
        syncOffButton = Theme_button(powerRow, "Off", (CardInner - 8) / 2, 34);
        lv_obj_add_event_cb(syncOffButton, onSyncPower, LV_EVENT_CLICKED, (void *)(intptr_t) false);

        syncButton = Theme_button(right, LV_SYMBOL_REFRESH "  Sync", CardInner, 38);
        lv_obj_add_event_cb(syncButton, onSync, LV_EVENT_CLICKED, nullptr);
    }

    void refreshAcPanel()
    {
        applyAcChip(panelChip);

        char buffer[48];
        const char *caption = formatAcSetpoint(buffer, sizeof(buffer));
        Theme_setText(panelBig, buffer);
        Theme_setText(panelMode, caption);
        lv_obj_set_style_text_color(panelBig, AcClient_targetEnabled() ? UI_COL_COOL : UI_COL_TEXT,
                                    LV_PART_MAIN);

        float roomTemperature = 0;
        if (AcClient_roomTemperature(roomTemperature))
        {
            char room[24];
            formatTemp(room, sizeof(room), roomTemperature, "\xC2\xB0");
            snprintf(buffer, sizeof(buffer), "Room %s", room);
            Theme_setText(panelRoom, buffer);
        }
        else
        {
            Theme_setText(panelRoom, "Room --");
        }

        snprintf(buffer, sizeof(buffer), "%d\xC2\xB0", syncTemp);
        Theme_setText(syncTempLabel, buffer);
        setToggleLit(syncOnButton, syncOn, UI_COL_COOL);
        setToggleLit(syncOffButton, !syncOn, UI_COL_TEXT);

        const bool showSynced = syncedAtMs && (millis() - syncedAtMs) < SyncedFeedbackMs;
        setButtonText(syncButton, showSynced ? LV_SYMBOL_OK "  Synced" : LV_SYMBOL_REFRESH "  Sync");
        lv_obj_set_style_text_color(syncButton, showSynced ? UI_COL_OK : UI_COL_TEXT, LV_PART_MAIN);
    }

#pragma endregion

#pragma region "AC - compact card"

    lv_obj_t *acChip = nullptr;
    lv_obj_t *acBig = nullptr;
    lv_obj_t *acCaption = nullptr;
    lv_obj_t *acAutoButton = nullptr;
    lv_obj_t *acSleepButton = nullptr;
    lv_obj_t *acAdjustButton = nullptr;
    lv_obj_t *acDoor = nullptr;
    lv_obj_t *acControls = nullptr;

    void buildAcCard(lv_obj_t *parent)
    {
        // The card is the power switch.
        acCard = makeCardButton(parent, UI_COL_W, LV_PCT(100));
        lv_obj_set_style_pad_row(acCard, 5, LV_PART_MAIN);
        lv_obj_add_event_cb(acCard, onAcCard, LV_EVENT_CLICKED, nullptr);

        // Title and state on separate rows: side by side, "Air Conditioning" and
        // a pill like "auto - cooling" do not fit across a 228px column.
        lv_obj_t *title = Theme_label(acCard, "Air Conditioning", &lv_font_montserrat_16, UI_COL_TEXT);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, CardInner);
        acChip = Theme_chip(acCard);

        acControls = Theme_column(acCard, CardInner, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(acControls, 1);
        lv_obj_set_flex_align(acControls, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(acControls, 5, LV_PART_MAIN);

        acBig = Theme_label(acControls, "--", &lv_font_montserrat_48, UI_COL_TEXT);
        acCaption = Theme_label(acControls, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        acDoor = Theme_label(acControls, "", &lv_font_montserrat_16, UI_COL_TEXT_FAINT);

        // Nested buttons: LVGL does not bubble their clicks, so none of these
        // also toggles the power.
        lv_obj_t *buttons = Theme_row(acControls, CardInner, LV_SIZE_CONTENT);
        const lv_coord_t buttonWidth = (CardInner - 12) / 3;

        acAutoButton = Theme_button(buttons, "Auto", buttonWidth, 38);
        lv_obj_add_event_cb(acAutoButton, onAcAuto, LV_EVENT_CLICKED, nullptr);

        acSleepButton = Theme_button(buttons, LV_SYMBOL_BELL, buttonWidth, 38);
        lv_obj_add_event_cb(acSleepButton, onSleepOpen, LV_EVENT_CLICKED, nullptr);

        acAdjustButton = Theme_button(buttons, LV_SYMBOL_SETTINGS, buttonWidth, 38);
        lv_obj_add_event_cb(acAdjustButton, onExpand, LV_EVENT_CLICKED, nullptr);
    }

    void refreshAcCard()
    {
        // The controls always stay on screen; when there is nothing to talk to
        // they are simply drawn inert.
        const Liveness live = livenessOf(TARGET_SLOT_AC);
        const bool enabled = live == CARD_LIVE;

        applyAcChip(acChip);
        setCardLit(acCard, enabled && AcClient_powerOn(), UI_COL_COOL);

        char buffer[64];
        const char *caption = live == CARD_UNBOUND ? "" : formatAcSetpoint(buffer, sizeof(buffer));
        Theme_setText(acBig, live == CARD_UNBOUND ? "--" : buffer);
        lv_obj_set_style_text_color(acBig,
                                    !enabled ? UI_COL_TEXT_FAINT
                                             : (AcClient_targetEnabled() ? UI_COL_COOL : UI_COL_TEXT),
                                    LV_PART_MAIN);

        // Setpoint caption, room temperature and any sleep timer share one line:
        // the column is 228px wide and only 230 tall.
        char line[96];
        int used = 0;
        if (live == CARD_UNBOUND)
        {
            used = snprintf(line, sizeof(line), "no device bound - tap to choose one");
        }
        else if (live == CARD_OFFLINE)
        {
            used = snprintf(line, sizeof(line), "%s is not answering",
                            Targets_device(TARGET_SLOT_AC).c_str());
        }
        else
        {
            used = snprintf(line, sizeof(line), "%s", caption);
            float roomTemperature = 0;
            if (AcClient_roomTemperature(roomTemperature))
            {
                char room[24];
                formatTemp(room, sizeof(room), roomTemperature, "\xC2\xB0");
                used += snprintf(line + used, sizeof(line) - used, " - room %s", room);
            }
        }

        const uint32_t remaining = enabled ? sleepRemaining() : 0;
        if (remaining)
        {
            snprintf(line + used, sizeof(line) - used, "\n" LV_SYMBOL_BELL " %luh %02lum",
                     (unsigned long)(remaining / 3600), (unsigned long)((remaining % 3600) / 60));
        }
        Theme_setText(acCaption, line);
        lv_obj_set_style_text_color(acCaption, live == CARD_OFFLINE ? UI_COL_DANGER : UI_COL_TEXT_FAINT,
                                    LV_PART_MAIN);

        applyButton(acAutoButton, enabled, AcClient_targetEnabled(), UI_COL_COOL);
        applyButton(acSleepButton, enabled, remaining != 0, UI_COL_WARN);
        applyButton(acAdjustButton, enabled, false, UI_COL_TEXT);

        switch (SensorBindings_doorState())
        {
        case DOOR_OPEN:
            Theme_setText(acDoor, LV_SYMBOL_WARNING "  Door open");
            lv_obj_set_style_text_color(acDoor, UI_COL_WARN, LV_PART_MAIN);
            break;
        case DOOR_CLOSED:
            Theme_setText(acDoor, LV_SYMBOL_OK "  Door closed");
            lv_obj_set_style_text_color(acDoor, UI_COL_OK, LV_PART_MAIN);
            break;
        case DOOR_UNKNOWN:
            Theme_setText(acDoor, "Door: no reading");
            lv_obj_set_style_text_color(acDoor, UI_COL_TEXT_FAINT, LV_PART_MAIN);
            break;
        default:
            Theme_setText(acDoor, "Door: pick a resource");
            lv_obj_set_style_text_color(acDoor, UI_COL_TEXT_FAINT, LV_PART_MAIN);
            break;
        }
    }

#pragma endregion

#pragma region "Light"

    lv_obj_t *lightCard = nullptr;
    lv_obj_t *lightTitle = nullptr;
    lv_obj_t *lightStateLabel = nullptr;
    lv_obj_t *lightDetail = nullptr;

    void onLightCard(lv_event_t *event)
    {
        switch (livenessOf(TARGET_SLOT_LIGHT))
        {
        case CARD_UNBOUND:
            // Nothing to toggle; the Resources screen is where a light gets
            // picked out of what the network actually publishes.
            Ui_show(UI_PAGE_SENSORS);
            return;
        case CARD_OFFLINE:
            return;
        default:
            break;
        }
        // The card flips at once -- the write is optimistic -- and the owner's
        // own state wins again when it arrives, or when the window closes.
        LightControl_toggle();
        Ui_markDirty();
    }

    void buildLightCard(lv_obj_t *parent)
    {
        lightCard = makeCardButton(parent, UI_COL_W, LV_SIZE_CONTENT);
        lv_obj_add_event_cb(lightCard, onLightCard, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *header = Theme_row(lightCard, CardInner, LV_SIZE_CONTENT);
        lightTitle = Theme_label(header, LV_SYMBOL_POWER "  Light", &lv_font_montserrat_16, UI_COL_TEXT);
        lightStateLabel = Theme_label(header, "", &lv_font_montserrat_16, UI_COL_TEXT_DIM);

        lightDetail = Theme_label(lightCard, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_label_set_long_mode(lightDetail, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lightDetail, CardInner);
    }

    void refreshLightCard()
    {
        const Liveness live = livenessOf(TARGET_SLOT_LIGHT);
        const bool enabled = live == CARD_LIVE;
        const LightState state = LightControl_state();
        const bool on = enabled && state == LIGHT_ON;

        setCardLit(lightCard, on, UI_COL_ACCENT);
        lv_obj_set_style_text_color(lightTitle,
                                    !enabled ? UI_COL_TEXT_FAINT : (on ? UI_COL_ACCENT : UI_COL_TEXT),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(lightStateLabel,
                                    !enabled ? UI_COL_TEXT_FAINT : (on ? UI_COL_ACCENT : UI_COL_TEXT_DIM),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(lightDetail, live == CARD_OFFLINE ? UI_COL_DANGER : UI_COL_TEXT_FAINT,
                                    LV_PART_MAIN);

        if (live == CARD_UNBOUND)
        {
            Theme_setText(lightStateLabel, "--");
            Theme_setText(lightDetail, "nothing bound - tap to pick a resource");
            return;
        }
        if (live == CARD_OFFLINE)
        {
            char detail[96];
            snprintf(detail, sizeof(detail), "%s is not answering",
                     Targets_device(TARGET_SLOT_LIGHT).c_str());
            Theme_setText(lightStateLabel, "--");
            Theme_setText(lightDetail, detail);
            return;
        }

        switch (state)
        {
        case LIGHT_ON:
            Theme_setText(lightStateLabel, "On");
            Theme_setText(lightDetail, "tap to toggle");
            break;
        case LIGHT_OFF:
            Theme_setText(lightStateLabel, "Off");
            Theme_setText(lightDetail, "tap to toggle");
            break;
        default:
        {
            // Bound and online, but the owner has published no state yet --
            // tapping is still allowed, the state just is not known.
            Theme_setText(lightStateLabel, "?");
            char detail[96];
            snprintf(detail, sizeof(detail), "waiting for \"%s\" from %s",
                     Targets_resource(TARGET_SLOT_LIGHT).c_str(),
                     Targets_device(TARGET_SLOT_LIGHT).c_str());
            Theme_setText(lightDetail, detail);
            break;
        }
        }
    }

#pragma endregion

#pragma region "RGB"

    lv_obj_t *rgbSwitch = nullptr;
    lv_obj_t *rgbSwatch = nullptr;
    lv_obj_t *rgbBrightness = nullptr;
    lv_obj_t *rgbStatus = nullptr;
    lv_obj_t *colorModal = nullptr;
    lv_obj_t *colorWheel = nullptr;

    const uint32_t ColorPresets[] = {
        0xFF9849, 0xFFFFFF, 0xFFD08A, 0xF2566A,
        0x37C978, 0x4FB0F5, 0x9B6BFF, 0xFF63C0};
    const int ColorPresetCount = sizeof(ColorPresets) / sizeof(ColorPresets[0]);

    void applyColor(uint32_t hex)
    {
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            return;
        }
        Rgb_setColour(hex & 0x00FFFFFF);
        lv_obj_set_style_bg_color(rgbSwatch, lv_color_hex(hex), LV_PART_MAIN);
        Ui_markDirty();
    }

    void onColorModalEvent(lv_event_t *event)
    {
        lv_obj_t *target = lv_event_get_target(event);
        if (target == colorWheel)
        {
            applyColor(lv_color_to32(lv_colorwheel_get_rgb(colorWheel)) & 0x00FFFFFF);
            return;
        }
        lv_obj_add_flag(colorModal, LV_OBJ_FLAG_HIDDEN);
    }

    void buildColorModal()
    {
        colorModal = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(colorModal);
        lv_obj_set_size(colorModal, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_set_style_bg_color(colorModal, UI_COL_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(colorModal, LV_OPA_90, LV_PART_MAIN);
        lv_obj_clear_flag(colorModal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(colorModal, LV_OBJ_FLAG_HIDDEN);

        // Wheel on the left, controls on the right, so the wheel can stay large
        // on a screen only 320px tall.
        lv_obj_t *panel = Theme_card(colorModal, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_center(panel);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(panel, 16, LV_PART_MAIN);

        colorWheel = lv_colorwheel_create(panel, true);
        lv_obj_set_size(colorWheel, 250, 250);
        lv_obj_add_event_cb(colorWheel, onColorModalEvent, LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *side = Theme_column(panel, 150, LV_SIZE_CONTENT);
        Theme_label(side, "Pick a colour", &lv_font_montserrat_16, UI_COL_TEXT);
        lv_obj_t *close = Theme_button(side, "Done", 140, 44);
        lv_obj_add_event_cb(close, onColorModalEvent, LV_EVENT_CLICKED, nullptr);
    }

    void onSwatchClicked(lv_event_t *event)
    {
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            return;
        }
        // The wheel opens on the colour at full brightness, not on the dimmed
        // value the device is actually showing -- picking a hue and setting a
        // level are separate choices here.
        lv_colorwheel_set_rgb(colorWheel, lv_color_hex(Rgb_colour()));
        lv_obj_clear_flag(colorModal, LV_OBJ_FLAG_HIDDEN);
    }

    void onPresetClicked(lv_event_t *event)
    {
        applyColor((uint32_t)(intptr_t)lv_event_get_user_data(event));
    }

    void onRgbToggle(lv_event_t *event)
    {
        if (applyingRemoteState || livenessOf(TARGET_SLOT_RGB) != CARD_LIVE)
        {
            return;
        }
        Rgb_setOn(lv_obj_has_state(rgbSwitch, LV_STATE_CHECKED));
        Ui_markDirty();
    }

    void onBrightnessReleased(lv_event_t *event)
    {
        if (applyingRemoteState || !Targets_configured(TARGET_SLOT_RGB))
        {
            return;
        }
        // Sent on release rather than on every value change: dragging a slider
        // emits dozens of events and each one would be an MQTT publish.
        Rgb_setBrightness((uint8_t)lv_slider_get_value(rgbBrightness));
        Ui_markDirty();
    }

    void buildRgbCard(lv_obj_t *parent)
    {
        lv_obj_t *card = Theme_card(parent, UI_COL_W, LV_SIZE_CONTENT);
        lv_obj_t *header = Theme_cardHeader(card, "RGB Light");

        rgbSwitch = lv_switch_create(header);
        lv_obj_set_size(rgbSwitch, 54, 30);
        lv_obj_set_style_bg_color(rgbSwitch, UI_COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(rgbSwitch, onRgbToggle, LV_EVENT_VALUE_CHANGED, nullptr);

        // Shown only when there is nothing to talk to; the controls below stay on
        // screen either way, just inert.
        rgbStatus = unboundNotice(card, "");

        // Wrapping, so the preset count is not tied to the column width.
        lv_obj_t *swatches = lv_obj_create(card);
        Theme_plainContainer(swatches);
        lv_obj_clear_flag(swatches, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(swatches, CardInner, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(swatches, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(swatches, 6, LV_PART_MAIN);

        // The current colour doubles as the way in to the full picker.
        rgbSwatch = lv_btn_create(swatches);
        lv_obj_remove_style_all(rgbSwatch);
        lv_obj_set_size(rgbSwatch, 40, 28);
        lv_obj_set_style_radius(rgbSwatch, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(rgbSwatch, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(rgbSwatch, UI_COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_border_width(rgbSwatch, 2, LV_PART_MAIN);
        lv_obj_add_event_cb(rgbSwatch, onSwatchClicked, LV_EVENT_CLICKED, nullptr);

        for (int i = 0; i < ColorPresetCount; ++i)
        {
            lv_obj_t *preset = lv_btn_create(swatches);
            lv_obj_remove_style_all(preset);
            lv_obj_set_size(preset, 28, 28);
            lv_obj_set_style_radius(preset, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(preset, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(preset, lv_color_hex(ColorPresets[i]), LV_PART_MAIN);
            lv_obj_add_event_cb(preset, onPresetClicked, LV_EVENT_CLICKED, (void *)(intptr_t)ColorPresets[i]);
        }

        rgbBrightness = lv_slider_create(card);
        lv_obj_set_width(rgbBrightness, CardInner);
        lv_slider_set_range(rgbBrightness, 0, 255);
        lv_obj_set_style_bg_color(rgbBrightness, UI_COL_SURFACE_ALT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(rgbBrightness, UI_COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(rgbBrightness, UI_COL_TEXT, LV_PART_KNOB);
        lv_obj_add_event_cb(rgbBrightness, onBrightnessReleased, LV_EVENT_RELEASED, nullptr);
    }

    void refreshRgbCard()
    {
        const Liveness live = livenessOf(TARGET_SLOT_RGB);
        const bool enabled = live == CARD_LIVE;

        setWidgetEnabled(rgbSwitch, enabled);
        setWidgetEnabled(rgbBrightness, enabled);

        if (enabled)
        {
            lv_obj_add_flag(rgbStatus, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            char detail[96];
            if (live == CARD_UNBOUND)
            {
                snprintf(detail, sizeof(detail), "nothing bound - pick a colour resource under Resources");
            }
            else
            {
                snprintf(detail, sizeof(detail), "%s is not answering",
                         Targets_device(TARGET_SLOT_RGB).c_str());
            }
            Theme_setText(rgbStatus, detail);
            lv_obj_set_style_text_color(rgbStatus, live == CARD_OFFLINE ? UI_COL_DANGER : UI_COL_TEXT_DIM,
                                        LV_PART_MAIN);
            lv_obj_clear_flag(rgbStatus, LV_OBJ_FLAG_HIDDEN);
            return;
        }

        applyingRemoteState = true;
        if (Rgb_on())
        {
            lv_obj_add_state(rgbSwitch, LV_STATE_CHECKED);
        }
        else
        {
            lv_obj_clear_state(rgbSwitch, LV_STATE_CHECKED);
        }

        // Leave the slider alone while it is being dragged, or the refresh would
        // yank the knob back under the finger.
        if (!lv_obj_has_state(rgbBrightness, LV_STATE_PRESSED))
        {
            lv_slider_set_value(rgbBrightness, Rgb_brightness(), LV_ANIM_OFF);
        }
        applyingRemoteState = false;

        lv_obj_set_style_bg_color(rgbSwatch, lv_color_hex(Rgb_colour()), LV_PART_MAIN);
    }

#pragma endregion

#pragma region "Weather"

    lv_obj_t *weatherIcon = nullptr;
    lv_obj_t *weatherTemp = nullptr;
    lv_obj_t *weatherCondition = nullptr;
    lv_obj_t *weatherRange = nullptr;

    void onWeatherClicked(lv_event_t *event)
    {
        Forecast_request();
    }

    void buildWeatherCard(lv_obj_t *parent)
    {
        lv_obj_t *card = Theme_card(parent, UI_COL_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, onWeatherClicked, LV_EVENT_CLICKED, nullptr);

        weatherIcon = lv_img_create(card);
        // 48, matching the art. The icons were rescaled from 64 to fit the
        // firmware into an OTA slot; a box larger than the image would not
        // enlarge it, it would just leave the card looking mis-centred.
        lv_obj_set_size(weatherIcon, 48, 48);

        lv_obj_t *column = Theme_column(card, CardInner - 54, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(column, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(column, 8, LV_PART_MAIN);

        weatherTemp = Theme_label(column, "--\xC2\xB0", &lv_font_montserrat_28, UI_COL_TEXT);
        weatherCondition = Theme_label(column, "waiting", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(weatherCondition, LV_LABEL_LONG_DOT);
        lv_obj_set_width(weatherCondition, CardInner - 62);
        weatherRange = Theme_label(column, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    }

    void refreshWeatherCard()
    {
        const ForecastData &data = Forecast_get();
        const lv_img_dsc_t *icon = Forecast_icon();

        if (icon)
        {
            lv_img_set_src(weatherIcon, icon);
            lv_obj_clear_flag(weatherIcon, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(weatherIcon, LV_OBJ_FLAG_HIDDEN);
        }

        char buffer[64];
        if (data.hasTemp)
        {
            formatTemp(buffer, sizeof(buffer), data.temp, "\xC2\xB0");
            Theme_setText(weatherTemp, buffer);
        }
        else
        {
            Theme_setText(weatherTemp, "--\xC2\xB0");
        }

        if (data.condition.length())
        {
            Theme_setText(weatherCondition, data.condition.c_str());
        }
        else
        {
            Theme_setText(weatherCondition, data.valid ? "" : "waiting");
        }

        if (data.hasRange)
        {
            char lo[16], hi[16];
            formatTemp(lo, sizeof(lo), data.minTemp, "\xC2\xB0");
            formatTemp(hi, sizeof(hi), data.maxTemp, "\xC2\xB0");
            if (data.hasRain)
            {
                snprintf(buffer, sizeof(buffer), "%s/%s  " LV_SYMBOL_TINT "%d%%", lo, hi, data.rainProb);
            }
            else
            {
                snprintf(buffer, sizeof(buffer), "%s / %s", lo, hi);
            }
            Theme_setText(weatherRange, buffer);
        }
        else
        {
            Theme_setText(weatherRange, "");
        }
    }

#pragma endregion
}

lv_obj_t *ScreenFocus_create(lv_obj_t *parent)
{
    page = lv_obj_create(parent);
    Theme_plainContainer(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_column(page, UI_GUTTER, LV_PART_MAIN);

    buildAcCard(page);

    sideColumn = lv_obj_create(page);
    Theme_plainContainer(sideColumn);
    lv_obj_set_size(sideColumn, UI_COL_W, LV_PCT(100));
    lv_obj_add_flag(sideColumn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sideColumn, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sideColumn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(sideColumn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sideColumn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sideColumn, UI_GUTTER, LV_PART_MAIN);

    buildWeatherCard(sideColumn);
    buildLightCard(sideColumn);
    buildRgbCard(sideColumn);

    // Built last so it stacks above both columns when shown.
    buildAcPanel(page);

    // Dialogs at boot rather than on first tap, so opening one never has to find
    // heap at a bad moment -- see the note on the bind dialog in ScreenSensors.
    buildColorModal();
    buildSleepModal();

    return page;
}

void ScreenFocus_collapse()
{
    if (!acPanel || lv_obj_has_flag(acPanel, LV_OBJ_FLAG_HIDDEN))
    {
        return;
    }
    lv_obj_add_flag(acPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(acCard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(sideColumn, LV_OBJ_FLAG_HIDDEN);
    Ui_markDirty();
}

void ScreenFocus_refresh()
{
    if (!lv_obj_has_flag(acPanel, LV_OBJ_FLAG_HIDDEN))
    {
        // The AC was unbound while its panel was open: nothing left to adjust.
        if (!Targets_configured(TARGET_SLOT_AC))
        {
            ScreenFocus_collapse();
        }
        else
        {
            refreshAcPanel();
            return;
        }
    }

    refreshAcCard();
    refreshWeatherCard();
    refreshLightCard();
    refreshRgbCard();
}
