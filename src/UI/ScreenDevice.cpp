#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Registry.h"
#include "../App/Targets.h"
#include "../App/Net.h"
#include "../App/Mqttp.h"

#include <boardstuff.h>
#include <ArduinoJson.h>

// One device, in detail: what it is bound to, what can be done to it, and what
// it says about itself.
//
// The explore section is the panel's cut-down answer to the backend's device
// page. The backend stores every answer from every device forever; this holds
// one answer, for one device, for as long as the page is open. Leaving frees
// it, and reopening asks again -- the data is live anyway.

namespace
{
    /// Three sections of the framework's own INFO document, which every
    /// NightMare device answers without a project resolver. Nothing
    /// device-specific is offered: a command implemented only in one firmware's
    /// local resolver would simply time out everywhere else.
    struct Query
    {
        const char *label;
        const char *command;
    };

    const Query Queries[] = {
        {"Boot", "INFO BOOT"},
        {"Hardware", "INFO HARDWARE"},
        {"System", "INFO SYSTEM"},
    };
    const int QueryCount = sizeof(Queries) / sizeof(Queries[0]);

    lv_obj_t *page = nullptr;
    lv_obj_t *titleLabel = nullptr;
    lv_obj_t *statusChip = nullptr;
    lv_obj_t *acButton = nullptr;
    lv_obj_t *unbindButton = nullptr;
    lv_obj_t *actionButton = nullptr;
    lv_obj_t *queryButtons[3] = {nullptr};
    lv_obj_t *resultTitle = nullptr;
    lv_obj_t *resultBody = nullptr;

    String deviceName;
    bool actionArmed = false;
    uint32_t lastSeenRevision = 0;

    void setButtonText(lv_obj_t *button, const char *text)
    {
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label)
        {
            lv_label_set_text(label, text);
        }
    }

#pragma region "Response rendering"

    /// LVGL's recolor markup is delimited by '#', so a '#' inside a value would
    /// be read as markup and swallow the rest of the line.
    String sanitize(const String &raw)
    {
        String out = raw;
        out.replace("#", "*");
        return out;
    }

    /// Flatten JSON into "key  value" lines, nesting keys as "parent.child".
    /// Keys are dimmed with recolor markup so the whole response can live in a
    /// single label -- one widget instead of a row pool of three per entry,
    /// which on this board is the difference that matters.
    void flatten(JsonVariantConst value, const String &prefix, String &out, int depth)
    {
        if (depth > 4)
        {
            return;
        }

        if (value.is<JsonObjectConst>())
        {
            for (JsonPairConst entry : value.as<JsonObjectConst>())
            {
                const String key = prefix.length() ? prefix + "." + entry.key().c_str()
                                                   : String(entry.key().c_str());
                flatten(entry.value(), key, out, depth + 1);
            }
            return;
        }

        if (value.is<JsonArrayConst>())
        {
            int index = 0;
            for (JsonVariantConst item : value.as<JsonArrayConst>())
            {
                flatten(item, prefix + "[" + index + "]", out, depth + 1);
                index++;
            }
            return;
        }

        out += "#8a97a8 ";
        out += sanitize(prefix);
        out += "#  ";
        out += sanitize(value.as<String>());
        out += "\n";
    }

    void renderResponse()
    {
        const MqttpState state = Mqttp_state();

        if (state == MQTTP_IDLE)
        {
            lv_label_set_text(resultTitle, "");
            lv_label_set_text(resultBody, "Pick one above to ask this device about itself.");
            return;
        }

        char heading[64];
        snprintf(heading, sizeof(heading), "%s", Mqttp_command().c_str());
        lv_label_set_text(resultTitle, heading);

        if (state == MQTTP_PENDING)
        {
            lv_label_set_text(resultBody, "Asking...");
            return;
        }
        if (state == MQTTP_TIMEOUT)
        {
            lv_label_set_text(resultBody,
                              "No answer. The device may be offline, or may not "
                              "implement this command.");
            return;
        }
        if (state == MQTTP_FAILED)
        {
            // Sanitized like everything else: recolor is on for this label, so
            // a '#' in the device's error text would be read as markup.
            lv_label_set_text(resultBody, sanitize(Mqttp_response()).c_str());
            return;
        }

        // Most answers are JSON objects; some (PING, plain acknowledgements)
        // are not. Try structured first, fall back to showing it verbatim.
        //
        // The document lives only for this render, which happens only when an
        // answer arrives, and it sizes itself as it parses rather than asking
        // for one block big enough for the largest reply the protocol allows.
        // If the heap cannot spare even that, ArduinoJson reports NoMemory and
        // the answer is shown as raw text -- degraded, never a crash.
        JsonDocument parseDoc;
        const DeserializationError error = deserializeJson(parseDoc, Mqttp_response());

        String text;
        if (error || parseDoc.isNull() ||
            (!parseDoc.is<JsonObjectConst>() && !parseDoc.is<JsonArrayConst>()))
        {
            text = sanitize(Mqttp_response());
        }
        else
        {
            flatten(parseDoc.as<JsonVariantConst>(), "", text, 0);
        }

        if (Mqttp_truncated())
        {
            text += "\n#ffc64b ...truncated at ";
            text += MQTTP_RESPONSE_MAX;
            text += " bytes#";
        }
        if (!text.length())
        {
            text = "(empty response)";
        }
        lv_label_set_text(resultBody, text.c_str());
    }

#pragma endregion

#pragma region "Events"

    void onBack(lv_event_t *event)
    {
        // The answer and the subscription are dropped by ScreenDevice_release(),
        // which Ui_show() calls on the way out.
        Ui_show(UI_PAGE_DEVICES);
    }

    /// The AC is the one role bound here, because it is the one that claims a
    /// whole device: its nine resources are a published contract, so naming the
    /// device names all of them. The light and the colour are single resources
    /// with no agreed names, so they are bound on the Resources screen instead,
    /// where what is actually being pointed at is visible.
    void onRole(lv_event_t *event)
    {
        const bool bind = (bool)(intptr_t)lv_event_get_user_data(event);
        Targets_setDevice(TARGET_SLOT_AC, bind ? deviceName : String(""));
        Ui_markDirty();
    }

    void onQuery(lv_event_t *event)
    {
        const int index = (int)(intptr_t)lv_event_get_user_data(event);
        if (index < 0 || index >= QueryCount)
        {
            return;
        }
        if (!Mqttp_request(deviceName, Queries[index].command))
        {
            // Nothing changed state, so the normal render would not run: say so
            // here, or the tap would look ignored.
            lv_label_set_text(resultTitle, Queries[index].command);
            lv_label_set_text(resultBody, "Could not ask: the broker is offline, or there is not "
                                          "enough free memory right now. Try again shortly.");
            return;
        }
        Ui_markDirty();
    }

    void onAction(lv_event_t *event)
    {
        const DeviceEntry *entry = Registry_findDevice(deviceName);
        if (!entry)
        {
            Ui_show(UI_PAGE_DEVICES);
            return;
        }

        // A wall panel gets brushed: reboot interrupts whatever the device is
        // doing and delete discards the network's record of it, so both want a
        // second deliberate tap.
        if (!actionArmed)
        {
            actionArmed = true;
            setButtonText(actionButton, "Tap again to confirm");
            return;
        }

        // Re-read rather than trusting what the button was built with: the
        // device may have come or gone between the two taps.
        if (entry->online)
        {
            Net_rebootDevice(deviceName);
        }
        else
        {
            Net_deleteDevice(deviceName);
            Ui_show(UI_PAGE_DEVICES);
            return;
        }

        actionArmed = false;
        Ui_markDirty();
    }

#pragma endregion

    lv_obj_t *sectionLabel(lv_obj_t *parent, const char *text)
    {
        return Theme_label(parent, text, &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    }

    /// Width inside a column card, after its padding.
    const lv_coord_t CardInner = UI_COL_W - 22;
}

lv_obj_t *ScreenDevice_create(lv_obj_t *parent)
{
    page = lv_obj_create(parent);
    Theme_plainContainer(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, UI_GUTTER, LV_PART_MAIN);

    // The header spans both columns: with only 230px of height, giving it a row
    // inside the left card left no room for the buttons underneath it.
    lv_obj_t *header = Theme_row(page, UI_CARD_W, LV_SIZE_CONTENT);
    lv_obj_t *back = Theme_button(header, LV_SYMBOL_LEFT, 44, 32);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);

    titleLabel = Theme_label(header, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLabel, UI_CARD_W - 200);
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    statusChip = Theme_chip(header);

    // An explicit height rather than flex_grow: the cards inside are LV_PCT(100)
    // tall, and a percentage of a height the layout has not resolved yet is
    // circular -- LVGL settles that unpredictably.
    const lv_coord_t headerHeight = 32;
    const lv_coord_t bodyHeight = UI_CONTENT_H - (2 * UI_GUTTER) - headerHeight - UI_GUTTER;
    lv_obj_t *body = Theme_row(page, UI_CARD_W, bodyHeight);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Left: what can be done to the device.
    lv_obj_t *left = Theme_card(body, UI_COL_W, LV_PCT(100));
    lv_obj_set_style_pad_row(left, 5, LV_PART_MAIN);

    sectionLabel(left, "Bind this panel's controls to");
    acButton = Theme_button(left, "Air conditioning", CardInner, 38);
    lv_obj_add_event_cb(acButton, onRole, LV_EVENT_CLICKED, (void *)(intptr_t) true);

    lv_obj_t *hint = Theme_label(left, "Lights are bound per resource, under Resources.",
                                 &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, CardInner);

    unbindButton = Theme_button(left, "Unbind the AC", CardInner, 32);
    lv_obj_add_event_cb(unbindButton, onRole, LV_EVENT_CLICKED, (void *)(intptr_t) false);

    // The one action on the device itself rather than on this panel.
    actionButton = Theme_button(left, "", CardInner, 40);
    lv_obj_add_event_cb(actionButton, onAction, LV_EVENT_CLICKED, nullptr);

    // Right: the questions, and the answer under them. They sit in a plain column
    // rather than inside the card, because the card scrolls -- put them in it and
    // a long reply would push the buttons off the top.
    lv_obj_t *rightColumn = Theme_column(body, UI_COL_W, LV_PCT(100));
    lv_obj_set_flex_align(rightColumn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_coord_t queryHeight = 38;
    lv_obj_t *queries = Theme_row(rightColumn, UI_COL_W, queryHeight);
    for (int i = 0; i < QueryCount; ++i)
    {
        queryButtons[i] = Theme_button(queries, Queries[i].label, (UI_COL_W - 12) / 3, queryHeight);
        lv_obj_add_event_cb(queryButtons[i], onQuery, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // An explicit height, for the same reason as `body` above: a percentage of a
    // height the layout has not resolved yet is circular.
    lv_obj_t *answer = Theme_card(rightColumn, UI_COL_W, bodyHeight - queryHeight - UI_GUTTER);
    lv_obj_add_flag(answer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(answer, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(answer, LV_SCROLLBAR_MODE_AUTO);

    resultTitle = Theme_label(answer, "", &lv_font_montserrat_14, UI_COL_ACCENT);

    resultBody = Theme_label(answer, "", &lv_font_montserrat_14, UI_COL_TEXT);
    lv_label_set_long_mode(resultBody, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(resultBody, true);
    lv_obj_set_width(resultBody, CardInner);

    return page;
}

void ScreenDevice_release()
{
    // An answer can be several KB of label text; it is only worth keeping while
    // someone is looking at it. The reply subscription goes with it.
    Mqttp_stopListening();
    if (resultBody)
    {
        lv_label_set_text(resultBody, "");
    }
    actionArmed = false;
    lastSeenRevision = 0;
}

void ScreenDevice_open(const String &name)
{
    deviceName = name;
    actionArmed = false;
    lastSeenRevision = 0;
    // Start listening now rather than when a button is tapped. A subscription
    // is only queued, not established, when the call returns -- and the device
    // answers in about a millisecond, so a request that subscribed for itself
    // would lose the reply. Opening the page buys the SUBACK the time it needs.
    // This also drops whatever the last device said, which is not about this one.
    Mqttp_listenTo(name);
}

void ScreenDevice_refresh()
{
    lv_label_set_text(titleLabel, deviceName.c_str());

    const DeviceEntry *entry = Registry_findDevice(deviceName);
    const bool online = entry && entry->online;

    if (!entry)
    {
        Theme_chipSet(statusChip, "gone", UI_COL_TEXT_FAINT);
    }
    else
    {
        Theme_chipSet(statusChip, online ? "online" : "offline",
                      online ? UI_COL_OK : UI_COL_DANGER);
    }

    // Only the AC is bound here, so only the AC is shown here. A device that
    // also owns the bound light or colour says nothing about it on this page --
    // that binding is a resource, and the Resources screen is where it lives.
    const bool isAc = Targets_isAc(deviceName);
    lv_obj_set_style_border_color(acButton, isAc ? UI_COL_ACCENT : UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_text_color(acButton, isAc ? UI_COL_ACCENT : UI_COL_TEXT, LV_PART_MAIN);

    if (isAc)
    {
        lv_obj_clear_flag(unbindButton, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(unbindButton, LV_OBJ_FLAG_HIDDEN);
    }

    // Reboot and delete are never both useful: rebooting something already
    // offline does nothing, and deleting something still publishing only makes
    // it reappear on its next message.
    if (!actionArmed)
    {
        setButtonText(actionButton, online ? LV_SYMBOL_REFRESH "  Reboot"
                                           : LV_SYMBOL_TRASH "  Delete");
    }
    lv_obj_set_style_text_color(actionButton, online ? UI_COL_WARN : UI_COL_DANGER, LV_PART_MAIN);
    lv_obj_set_style_border_color(actionButton, online ? UI_COL_WARN : UI_COL_DANGER, LV_PART_MAIN);

    // Re-rendering the response means reparsing several KB, so only do it when
    // something actually moved.
    const uint32_t revision = Mqttp_revision();
    if (revision != lastSeenRevision)
    {
        lastSeenRevision = revision;
        renderResponse();
    }
}
