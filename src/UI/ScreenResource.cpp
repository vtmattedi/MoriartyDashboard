#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"
#include "Pager.h"

#include "App/Registry.h"
#include "App/SensorBindings.h"
#include "App/Targets.h"

#include <boardstuff.h>
#include <ArduinoJson.h>

// One resource, in detail, and the page where it is given a job.
//
// A resource protocol addresses a capability as `<device>/resource/<name>`, and
// nothing obliges a device to call its light "light". So rather than assuming
// names, the panel shows what is actually being published and asks what it is
// for. Every role the panel has is bound from here.
//
// The air conditioning is the odd one out and says so on the button: it is not
// one resource but a contract of nine, so choosing it binds the resource's
// *device*, not the resource. It is offered here anyway because this is where
// someone looking at `ac_power` on some device will be standing.
//
// The page reads the registry on every refresh rather than copying a snapshot
// at open time: a resource's value changes while it is on screen, which is half
// of what makes the reading worth looking at before binding it.

namespace
{
    lv_obj_t *page = nullptr;
    lv_obj_t *titleLabel = nullptr;
    lv_obj_t *statusChip = nullptr;
    lv_obj_t *detailBody = nullptr;
    lv_obj_t *valueLabel = nullptr;
    Pager pager;

    lv_obj_t *lightButton = nullptr;
    lv_obj_t *doorOpenButton = nullptr;
    lv_obj_t *doorClosedButton = nullptr;
    lv_obj_t *restButton = nullptr;
    lv_obj_t *clearButton = nullptr;

    std::string deviceName;
    std::string resourceName;

    /// Width inside a column card, after its padding.
    const lv_coord_t CardInner = UI_CARD_INNER;

    enum Job
    {
        JOB_LIGHT = 0,
        JOB_DOOR_ONE_OPEN,
        JOB_DOOR_ONE_CLOSED,
        JOB_REST,
        JOB_CLEAR
    };

    /// LVGL's recolor markup is delimited by '#'. Device and resource names
    /// cannot contain one -- it is an MQTT wildcard, not allowed in a published
    /// topic -- but a value can.
    std::string sanitize(const std::string &raw)
    {
        std::string out = raw;
        Text::replaceAll(out, "#", "*");
        return out;
    }

#pragma region "Struct values"

    // The Resources list shows a JSON value as just "struct"; this page is the
    // "see more". It is rendered the same way the device page renders an MQTTP
    // answer -- flattened to dimmed "key  value" lines, nested keys joined with
    // a dot -- so one label holds the whole document instead of a pool of row
    // widgets, which on this board is the difference that matters.
    //
    // ScreenDevice.cpp has its own copy of this. The two are not shared because
    // they size their parse pools against different limits: that one against
    // MQTTP_RESPONSE_MAX, this one against the resource payload cap.

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

        out += "\n#8a97a8 ";
        out += sanitize(prefix);
        out += "#  ";
        out += sanitize(value.as<std::string>());
    }

    /// The last value formatted, and what it came out as. Parsing several KB on
    /// every refresh -- which runs whenever anything in the registry moves --
    /// would be steady heap churn for a document that rarely changes, so the
    /// result is kept until the raw value does.
    std::string cachedRaw;
    std::string cachedLines;
    bool cachedIsStruct = false;
    bool cacheValid = false;

    void formatValue(const std::string &raw)
    {
        if (cacheValid && raw == cachedRaw)
        {
            return;
        }
        cacheValid = true;
        cachedRaw = raw;
        cachedLines = "";
        cachedIsStruct = Ui_valueIsStruct(raw);
        if (!cachedIsStruct)
        {
            return;
        }

        // The document lives only for this format, which happens only when the
        // value changes, and it sizes itself as it parses. If the heap cannot
        // spare even that, ArduinoJson reports NoMemory and the value is shown
        // verbatim -- degraded, never a crash.
        JsonDocument doc;
        if (deserializeJson(doc, raw) != DeserializationError::Ok ||
            (!doc.is<JsonObjectConst>() && !doc.is<JsonArrayConst>()))
        {
            // It opened like a document but is not one. Say so rather than
            // claiming a structure that could not be read.
            cachedIsStruct = false;
            return;
        }
        flatten(doc.as<JsonVariantConst>(), "", cachedLines, 0);
    }

    void forgetCachedValue()
    {
        cachedRaw = "";
        cachedLines = "";
        cachedIsStruct = false;
        cacheValid = false;
    }

#pragma endregion

    /// The job this resource currently does, or nullptr. Roles come first: a
    /// resource the panel drives is the more interesting fact about it.
    const char *currentJob()
    {
        if (Targets_matches(TARGET_SLOT_LIGHT, deviceName, resourceName))
        {
            return "light switch";
        }
        if (SensorBindings_matches(SENSOR_SLOT_DOOR, deviceName, resourceName))
        {
            return SensorBindings_inverted(SENSOR_SLOT_DOOR) ? "door (1 = closed)"
                                                             : "door (1 = open)";
        }
        if (SensorBindings_matches(SENSOR_SLOT_REST, deviceName, resourceName))
        {
            return "resting screen";
        }
        return nullptr;
    }

#pragma region "Events"

    void onBack(lv_event_t *event)
    {
        Ui_show(UI_PAGE_SENSORS);
    }

    void onJob(lv_event_t *event)
    {
        if (!deviceName.length() || !resourceName.length())
        {
            return;
        }

        switch ((Job)(intptr_t)lv_event_get_user_data(event))
        {
        case JOB_LIGHT:
            Targets_set(TARGET_SLOT_LIGHT, deviceName, resourceName);
            break;
        case JOB_DOOR_ONE_OPEN:
            SensorBindings_set(SENSOR_SLOT_DOOR, deviceName, resourceName, false);
            break;
        case JOB_DOOR_ONE_CLOSED:
            SensorBindings_set(SENSOR_SLOT_DOOR, deviceName, resourceName, true);
            break;
        case JOB_REST:
            SensorBindings_set(SENSOR_SLOT_REST, deviceName, resourceName, false);
            break;
        case JOB_CLEAR:
            for (int slot = 0; slot < SENSOR_SLOT_COUNT; ++slot)
            {
                if (SensorBindings_matches((SensorSlot)slot, deviceName, resourceName))
                {
                    SensorBindings_clear((SensorSlot)slot);
                }
            }
            if (Targets_matches(TARGET_SLOT_LIGHT, deviceName, resourceName))
            {
                Targets_set(TARGET_SLOT_LIGHT, "", "");
            }
            break;
        }
        Ui_markDirty();
    }

#pragma endregion

    lv_obj_t *jobButton(lv_obj_t *parent, const char *text, Job job, lv_coord_t width, lv_coord_t height)
    {
        lv_obj_t *button = Theme_button(parent, text, width, height);
        lv_obj_set_style_text_align(lv_obj_get_child(button, 0), LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_event_cb(button, onJob, LV_EVENT_CLICKED, (void *)(intptr_t)job);
        return button;
    }

    /// Lit when this resource already holds that job, so the page says what is
    /// bound as well as offering to change it.
    void markButton(lv_obj_t *button, bool active)
    {
        lv_obj_set_style_border_color(button, active ? UI_COL_ACCENT : UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_text_color(button, active ? UI_COL_ACCENT : UI_COL_TEXT, LV_PART_MAIN);
    }
}

lv_obj_t *ScreenResource_create(lv_obj_t *parent)
{
    Pager_create(pager, parent, 2);

    // --- Page 1, "Value": what it reads, big, and where it comes from.
    lv_obj_t *valuePage = pager.page[0];
    lv_obj_set_flex_flow(valuePage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(valuePage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(valuePage, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(valuePage, UI_GUTTER, LV_PART_MAIN);

    const lv_coord_t heroHeight = 150;
    lv_obj_t *hero = Theme_card(valuePage, UI_CARD_W, heroHeight);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hero, 4, LV_PART_MAIN);

    titleLabel = Theme_label(hero, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLabel, CardInner);
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    valueLabel = Theme_label(hero, "--", &lv_font_montserrat_48, UI_COL_ACCENT);
    lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_size(valueLabel, CardInner, 56);
    lv_obj_set_style_text_align(valueLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    statusChip = Theme_chip(hero);

    // Where it comes from, and (for a document) its fields. The one place a
    // scroll is allowed: a document's length is not ours to choose, and only
    // this card's area redraws as it moves.
    const lv_coord_t detailHeight = UI_PAGE_H_PAGER - heroHeight - (3 * UI_GUTTER);
    lv_obj_t *detail = Theme_card(valuePage, UI_CARD_W, detailHeight);
    Theme_scrollVertical(detail, true);
    detailBody = Theme_label(detail, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
    lv_label_set_long_mode(detailBody, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(detailBody, true);
    lv_obj_set_width(detailBody, CardInner);
    lv_obj_set_style_text_line_space(detailBody, 3, LV_PART_MAIN);

    // --- Page 2, "Use as": big buttons, two across, lit when this resource
    // already has that job. The AC, PC link and RGB are bound to whole devices
    // on the device page, so they are not offered here.
    lv_obj_t *usePage = pager.page[1];
    lv_obj_set_flex_flow(usePage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(usePage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(usePage, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(usePage, UI_GUTTER, LV_PART_MAIN);

    lv_obj_t *jobs = Theme_card(usePage, UI_CARD_W, UI_PAGE_H_PAGER - 2 * UI_GUTTER);
    lv_obj_set_style_pad_row(jobs, 8, LV_PART_MAIN);
    Theme_cardHeader(jobs, LV_SYMBOL_SETTINGS "  Use this as");

    const lv_coord_t half = (CardInner - 8) / 2;
    const lv_coord_t big = 64;
    lv_obj_t *row1 = Theme_row(jobs, CardInner, big);
    lightButton = jobButton(row1, LV_SYMBOL_CHARGE "\nLight switch", JOB_LIGHT, half, big);
    restButton = jobButton(row1, LV_SYMBOL_EYE_CLOSE "\nResting screen", JOB_REST, half, big);

    // Polarity is part of the door choice, not a separate setting: a reed
    // switch reports 1 for open or for closed purely depending on its wiring,
    // and the person picking it is the one who knows which.
    Theme_label(jobs, "Door sensor, where 1 / true means:", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_t *row2 = Theme_row(jobs, CardInner, big);
    doorOpenButton = jobButton(row2, LV_SYMBOL_OK "\nOpen", JOB_DOOR_ONE_OPEN, half, big);
    doorClosedButton = jobButton(row2, LV_SYMBOL_CLOSE "\nClosed", JOB_DOOR_ONE_CLOSED, half, big);

    clearButton = jobButton(jobs, "Stop using this", JOB_CLEAR, CardInner, 48);
    lv_obj_set_style_text_color(clearButton, UI_COL_DANGER, LV_PART_MAIN);

    lv_obj_t *hint = Theme_label(jobs, "The AC, the PC link and the RGB light are set on the device's page.",
                                 &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, CardInner);

    return pager.root;
}

void ScreenResource_open(const std::string &device, const std::string &resource)
{
    deviceName = device;
    resourceName = resource;
    // Whatever the last resource read is not this one's.
    forgetCachedValue();
    Pager_go(pager, 0);
}

void ScreenResource_release()
{
    // A flattened status document is a couple of KB of std::string held for a page
    // that is almost never open. It costs nothing to rebuild on the way back in.
    forgetCachedValue();
}

void ScreenResource_refresh()
{
    Theme_setText(titleLabel, resourceName.c_str());

    const char *job = currentJob();
    if (job)
    {
        Theme_chipSet(statusChip, job, UI_COL_COOL);
    }
    else if (Targets_isAc(deviceName))
    {
        // Not this resource's own job, but the reason it is already being read.
        Theme_chipSet(statusChip, "on the AC", UI_COL_TEXT_DIM);
    }
    else
    {
        Theme_chipSet(statusChip, "unused", UI_COL_TEXT_FAINT);
    }

    const DeviceEntry *entry = Registry_findDevice(deviceName);
    const std::string value = Registry_resourceValue(deviceName, resourceName);

    // Read fresh each time: the whole point of looking at a resource before
    // binding it is to see what it currently reads.
    uint32_t lastSeen = 0;
    for (int i = 0; i < Registry_resourceCount(); ++i)
    {
        const ResourceEntry *candidate = Registry_resource(i);
        if (candidate && candidate->device == deviceName && candidate->resource == resourceName)
        {
            lastSeen = candidate->lastSeen;
            break;
        }
    }

    char age[24];
    Ui_formatAge(lastSeen, age, sizeof(age));
    formatValue(value);

    // The value, large. A short scalar gets the big font; a longer one a smaller
    // font; a document is summarised here and its fields listed below.
    const std::string shown = cachedIsStruct ? std::string("{ }") : sanitize(value);
    const size_t length = shown.length();
    lv_obj_set_style_text_font(valueLabel, length <= 7 ? &lv_font_montserrat_48 : (length <= 14 ? &lv_font_montserrat_28 : &lv_font_montserrat_16),
                               LV_PART_MAIN);
    Theme_setText(valueLabel, value.length() ? shown.c_str() : "--");

    // Where it comes from, then -- for a document -- its fields.
    std::string text;
    text += "#8a97a8 updated#  ";
    text += age;
    text += "\n#8a97a8 device#  ";
    text += sanitize(deviceName);
    text += "  (";
    text += entry ? (entry->online ? "online" : "offline") : "gone";
    text += ")\n#8a97a8 topic#  ";
    text += sanitize(deviceName);
    text += "/resource/";
    text += sanitize(resourceName);
    text += "/state";
    if (cachedIsStruct)
    {
        text += "\n";
        text += cachedLines; // each line already begins with its own newline
    }
    Theme_setText(detailBody, text.c_str());

    markButton(lightButton, Targets_matches(TARGET_SLOT_LIGHT, deviceName, resourceName));

    const bool isDoor = SensorBindings_matches(SENSOR_SLOT_DOOR, deviceName, resourceName);
    markButton(doorOpenButton, isDoor && !SensorBindings_inverted(SENSOR_SLOT_DOOR));
    markButton(doorClosedButton, isDoor && SensorBindings_inverted(SENSOR_SLOT_DOOR));
    markButton(restButton, SensorBindings_matches(SENSOR_SLOT_REST, deviceName, resourceName));

    if (job)
    {
        lv_obj_clear_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
    }
}
