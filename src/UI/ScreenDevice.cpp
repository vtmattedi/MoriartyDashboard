#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"

#include "App/Registry.h"
#include "App/Targets.h"
#include "App/Net.h"
#include "App/Mqttp.h"

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
    lv_obj_t *pcButton = nullptr;
    lv_obj_t *rgbButton = nullptr;
    lv_obj_t *actionButton = nullptr;
    lv_obj_t *queryButtons[3] = {nullptr};
    lv_obj_t *resultTitle = nullptr;
    lv_obj_t *resultBody = nullptr;

    std::string deviceName;
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
    std::string sanitize(const std::string &raw)
    {
        std::string out = raw;
        Text::replaceAll(out, "#", "*");
        return out;
    }

    /// Flatten JSON into "key  value" lines, nesting keys as "parent.child".
    /// Keys are dimmed with recolor markup so the whole response can live in a
    /// single label -- one widget instead of a row pool of three per entry,
    /// which on this board is the difference that matters.
    void flatten(JsonVariantConst value, const std::string &prefix, std::string &out, int depth)
    {
        if (depth > 4)
        {
            return;
        }

        if (value.is<JsonObjectConst>())
        {
            for (JsonPairConst entry : value.as<JsonObjectConst>())
            {
                const std::string key = prefix.length() ? prefix + "." + entry.key().c_str()
                                                   : std::string(entry.key().c_str());
                flatten(entry.value(), key, out, depth + 1);
            }
            return;
        }

        if (value.is<JsonArrayConst>())
        {
            int index = 0;
            for (JsonVariantConst item : value.as<JsonArrayConst>())
            {
                flatten(item, prefix + "[" + std::to_string(index) + "]", out, depth + 1);
                index++;
            }
            return;
        }

        out += "#8a97a8 ";
        out += sanitize(prefix);
        out += "#  ";
        out += sanitize(value.as<std::string>());
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

        std::string text;
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

    void onResources(lv_event_t *event)
    {
        ScreenSensors_open(deviceName);
    }

    void onBack(lv_event_t *event)
    {
        // The answer and the subscription are dropped by ScreenDevice_release(),
        // which Ui_show() calls on the way out.
        Ui_show(UI_PAGE_DEVICES);
    }

    /// The AC, the PC link and the RGB light are the roles bound here: each is a
    /// device with a published contract (the AC's resources, the PC's, an
    /// `rgb` colour), so naming the device names all of it. The plain light
    /// is a single resource with no agreed name, so it is bound on the
    /// resource's own page, where what is being pointed at is visible.
    ///
    /// Each button is a toggle: lit means this device fills that role, and
    /// tapping a lit one takes the role away again.
    void onRole(lv_event_t *event)
    {
        const TargetSlot slot = (TargetSlot)(intptr_t)lv_event_get_user_data(event);
        const bool bound = Targets_configured(slot) && Targets_device(slot) == deviceName;
        if (bound)
        {
            Targets_setDevice(slot, "");
        }
        else if (Targets_defaultResource(slot)[0])
        {
            // A device role is the contract's own resource name, whatever the
            // slot pointed at before.
            Targets_set(slot, deviceName, Targets_defaultResource(slot));
        }
        else
        {
            Targets_setDevice(slot, deviceName);
        }
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
            lv_label_set_text(resultBody, "Could not ask: the gateway is offline, or there is not "
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

    /// Width inside a card, after its padding.
    const lv_coord_t CardInner = UI_CARD_INNER;
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

    // Top to bottom: the header, what can be done to the device, the three
    // questions, and the answer -- which takes whatever height is left and
    // scrolls inside it, so a long reply never pushes the buttons off the top.
    lv_obj_t *header = Theme_row(page, UI_CARD_W, LV_SIZE_CONTENT);
    lv_obj_t *back = Theme_button(header, LV_SYMBOL_LEFT, 44, 32);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);

    titleLabel = Theme_label(header, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLabel, UI_CARD_W - 150);
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    statusChip = Theme_chip(header);

    // What can be done to the device. Every height in this card is explicit,
    // hint included, so the answer card below can be given exactly the rest of
    // the page: a flex_grow there would have LVGL sizing a scrolling card
    // against a height it is still working out.
    const lv_coord_t headerHeight = 32;
    const lv_coord_t actionsRowGap = 5;
    const lv_coord_t sectionHeight = 17;
    const lv_coord_t hintHeight = 17;
    const lv_coord_t actionButtonHeight = 38;
    const lv_coord_t actionsHeight = 22 + sectionHeight + hintHeight +
                                     (2 * actionButtonHeight) + (3 * actionsRowGap);

    lv_obj_t *left = Theme_card(page, UI_CARD_W, actionsHeight);
    lv_obj_set_style_pad_row(left, actionsRowGap, LV_PART_MAIN);

    lv_obj_t *section = sectionLabel(left, "Bind this panel's controls to");
    lv_obj_set_height(section, sectionHeight);

    // The two whole-device roles, side by side.
    const lv_coord_t half = (CardInner - 8) / 2;
    lv_obj_t *roleRow = Theme_row(left, CardInner, actionButtonHeight);
    const lv_coord_t third = (CardInner - 12) / 3;
    acButton = Theme_button(roleRow, "AC", third, actionButtonHeight);
    lv_obj_add_event_cb(acButton, onRole, LV_EVENT_CLICKED, (void *)(intptr_t)TARGET_SLOT_AC);

    pcButton = Theme_button(roleRow, "PC", third, actionButtonHeight);
    lv_obj_add_event_cb(pcButton, onRole, LV_EVENT_CLICKED, (void *)(intptr_t)TARGET_SLOT_PC);

    rgbButton = Theme_button(roleRow, "Bed", third, actionButtonHeight);
    lv_obj_add_event_cb(rgbButton, onRole, LV_EVENT_CLICKED, (void *)(intptr_t)TARGET_SLOT_RGB);

    lv_obj_t *hint = Theme_label(left, "Lit is bound; tap again to unbind.",
                                 &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
    lv_obj_set_size(hint, CardInner, hintHeight);

    // Its resources, and the one action on the device itself rather than on
    // this panel.
    lv_obj_t *lowerRow = Theme_row(left, CardInner, actionButtonHeight);
    lv_obj_t *resourcesButton = Theme_button(lowerRow, LV_SYMBOL_LIST "  Resources", half, actionButtonHeight);
    lv_obj_add_event_cb(resourcesButton, onResources, LV_EVENT_CLICKED, nullptr);
    actionButton = Theme_button(lowerRow, "", half, actionButtonHeight);
    lv_obj_add_event_cb(actionButton, onAction, LV_EVENT_CLICKED, nullptr);

    // The questions sit outside the answer card, because the card scrolls.
    const lv_coord_t queryHeight = 38;
    lv_obj_t *queries = Theme_row(page, UI_CARD_W, queryHeight);
    for (int i = 0; i < QueryCount; ++i)
    {
        queryButtons[i] = Theme_button(queries, Queries[i].label, (UI_CARD_W - 12) / 3, queryHeight);
        lv_obj_add_event_cb(queryButtons[i], onQuery, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    const lv_coord_t answerHeight = UI_PAGE_H - headerHeight - actionsHeight - queryHeight -
                                    (3 * UI_GUTTER);
    lv_obj_t *answer = Theme_card(page, UI_CARD_W, answerHeight);
    Theme_scrollVertical(answer, true);

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

void ScreenDevice_open(const std::string &name)
{
    deviceName = name;
    // Receive this device's resources while its pages are up.
    Net_watchDevice(name);
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

    // Only whole-device roles are bound here, so only those are shown here. A
    // device that also owns the bound light or colour says nothing about it on
    // this page -- that binding is a resource, and the Resources screen is
    // where it lives.
    const bool isAc = Targets_isAc(deviceName);
    lv_obj_set_style_border_color(acButton, isAc ? UI_COL_ACCENT : UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_text_color(acButton, isAc ? UI_COL_ACCENT : UI_COL_TEXT, LV_PART_MAIN);

    const bool isPc = Targets_configured(TARGET_SLOT_PC) &&
                      Targets_device(TARGET_SLOT_PC) == deviceName;
    lv_obj_set_style_border_color(pcButton, isPc ? UI_COL_ACCENT : UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_text_color(pcButton, isPc ? UI_COL_ACCENT : UI_COL_TEXT, LV_PART_MAIN);

    const bool isRgb = Targets_configured(TARGET_SLOT_RGB) && Targets_device(TARGET_SLOT_RGB) == deviceName;
    lv_obj_set_style_border_color(rgbButton, isRgb ? UI_COL_ACCENT : UI_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_text_color(rgbButton, isRgb ? UI_COL_ACCENT : UI_COL_TEXT, LV_PART_MAIN);

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
