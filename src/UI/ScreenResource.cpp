#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Registry.h"
#include "../App/SensorBindings.h"
#include "../App/Targets.h"

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

    lv_obj_t *lightButton = nullptr;
    lv_obj_t *rgbButton = nullptr;
    lv_obj_t *doorOpenButton = nullptr;
    lv_obj_t *doorClosedButton = nullptr;
    lv_obj_t *restButton = nullptr;
    lv_obj_t *acButton = nullptr;
    lv_obj_t *clearButton = nullptr;

    String deviceName;
    String resourceName;

    /// Width inside a column card, after its padding.
    const lv_coord_t CardInner = UI_COL_W - 22;

    enum Job
    {
        JOB_LIGHT = 0,
        JOB_RGB,
        JOB_DOOR_ONE_OPEN,
        JOB_DOOR_ONE_CLOSED,
        JOB_REST,
        JOB_AC,
        JOB_CLEAR
    };

    /// LVGL's recolor markup is delimited by '#'. Device and resource names
    /// cannot contain one -- it is an MQTT wildcard, not allowed in a published
    /// topic -- but a value can.
    String sanitize(const String &raw)
    {
        String out = raw;
        out.replace("#", "*");
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

        out += "\n#8a97a8 ";
        out += sanitize(prefix);
        out += "#  ";
        out += sanitize(value.as<String>());
    }

    /// The last value formatted, and what it came out as. Parsing several KB on
    /// every refresh -- which runs whenever anything in the registry moves --
    /// would be steady heap churn for a document that rarely changes, so the
    /// result is kept until the raw value does.
    String cachedRaw;
    String cachedLines;
    bool cachedIsStruct = false;
    bool cacheValid = false;

    void formatValue(const String &raw)
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
        if (Targets_matches(TARGET_SLOT_RGB, deviceName, resourceName))
        {
            return "RGB colour";
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
        case JOB_RGB:
            Targets_set(TARGET_SLOT_RGB, deviceName, resourceName);
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
        case JOB_AC:
            // The device, not the resource -- see the note at the top.
            Targets_setDevice(TARGET_SLOT_AC, deviceName);
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
            if (Targets_matches(TARGET_SLOT_RGB, deviceName, resourceName))
            {
                Targets_set(TARGET_SLOT_RGB, "", "");
            }
            break;
        }
        Ui_markDirty();
    }

#pragma endregion

    lv_obj_t *jobButton(lv_obj_t *parent, const char *text, Job job, lv_coord_t width)
    {
        lv_obj_t *button = Theme_button(parent, text, width, 34);
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
    page = lv_obj_create(parent);
    Theme_plainContainer(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, UI_GUTTER, LV_PART_MAIN);

    // The header spans both columns, as on the device page: 230px of height
    // leaves no room for a title row inside a card as well as its contents.
    lv_obj_t *header = Theme_row(page, UI_CARD_W, LV_SIZE_CONTENT);
    lv_obj_t *back = Theme_button(header, LV_SYMBOL_LEFT, 44, 32);
    lv_obj_add_event_cb(back, onBack, LV_EVENT_CLICKED, nullptr);

    titleLabel = Theme_label(header, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLabel, UI_CARD_W - 200);
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    statusChip = Theme_chip(header);

    // An explicit height rather than flex_grow: the cards inside are
    // LV_PCT(100) tall, and a percentage of a height the layout has not
    // resolved yet is circular -- LVGL settles that unpredictably.
    const lv_coord_t headerHeight = 32;
    const lv_coord_t bodyHeight = UI_CONTENT_H - (2 * UI_GUTTER) - headerHeight - UI_GUTTER;
    lv_obj_t *body = Theme_row(page, UI_CARD_W, bodyHeight);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Left: what this resource is.
    lv_obj_t *left = Theme_card(body, UI_COL_W, LV_PCT(100));
    lv_obj_add_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(left, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(left, LV_SCROLLBAR_MODE_AUTO);
    Theme_cardHeader(left, "Resource");

    detailBody = Theme_label(left, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
    lv_label_set_long_mode(detailBody, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(detailBody, true);
    lv_obj_set_width(detailBody, CardInner);
    lv_obj_set_style_text_line_space(detailBody, 2, LV_PART_MAIN);

    // Right: what it can be used for. Scrollable because seven buttons at a
    // fingertip's height is a little more than 230px.
    lv_obj_t *right = Theme_card(body, UI_COL_W, LV_PCT(100));
    lv_obj_set_style_pad_row(right, 4, LV_PART_MAIN);
    lv_obj_add_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(right, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(right, LV_SCROLLBAR_MODE_AUTO);
    Theme_cardHeader(right, "Use this as");

    // Paired across rows rather than stacked: seven full-width buttons are
    // taller than the 190px this card gets, and a list of jobs that has to be
    // scrolled to be seen is a list that hides half of itself.
    const lv_coord_t half = (CardInner - 8) / 2;

    lv_obj_t *lightRow = Theme_row(right, CardInner, LV_SIZE_CONTENT);
    lightButton = jobButton(lightRow, "Light", JOB_LIGHT, half);
    rgbButton = jobButton(lightRow, "RGB colour", JOB_RGB, half);

    // Polarity is part of the door choice, not a separate setting: a reed
    // switch reports 1 for open or for closed purely depending on its wiring,
    // and the person picking it is the one who knows which.
    Theme_label(right, "Door, where 1 / true is:", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
    lv_obj_t *doorRow = Theme_row(right, CardInner, LV_SIZE_CONTENT);
    doorOpenButton = jobButton(doorRow, "Open", JOB_DOOR_ONE_OPEN, half);
    doorClosedButton = jobButton(doorRow, "Closed", JOB_DOOR_ONE_CLOSED, half);

    // "AC (device)" binds the resource's device, not the resource -- see the
    // note at the top of the file. The label says so, because it is the one
    // button here that does not do what the page is about.
    lv_obj_t *lastRow = Theme_row(right, CardInner, LV_SIZE_CONTENT);
    restButton = jobButton(lastRow, "Resting", JOB_REST, half);
    acButton = jobButton(lastRow, "AC (device)", JOB_AC, half);

    clearButton = jobButton(right, "Stop using this", JOB_CLEAR, CardInner);
    lv_obj_set_style_text_color(clearButton, UI_COL_DANGER, LV_PART_MAIN);

    return page;
}

void ScreenResource_open(const String &device, const String &resource)
{
    deviceName = device;
    resourceName = resource;
    // Whatever the last resource read is not this one's.
    forgetCachedValue();
}

void ScreenResource_release()
{
    // A flattened status document is a couple of KB of String held for a page
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
    const String value = Registry_resourceValue(deviceName, resourceName);

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

    // Metadata first and the value last, because the value is the only part
    // whose length is not known: a status document runs to a dozen lines, and
    // the card scrolls, so anything below it would be the part that gets
    // pushed out of sight.
    String text;
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

    text += "\n\n#8a97a8 value#  ";
    if (!value.length())
    {
        text += "--";
    }
    else if (cachedIsStruct)
    {
        text += "struct";
        text += cachedLines; // each line already begins with its own newline
    }
    else
    {
        text += sanitize(value);
    }
    Theme_setText(detailBody, text.c_str());

    markButton(lightButton, Targets_matches(TARGET_SLOT_LIGHT, deviceName, resourceName));
    markButton(rgbButton, Targets_matches(TARGET_SLOT_RGB, deviceName, resourceName));

    const bool isDoor = SensorBindings_matches(SENSOR_SLOT_DOOR, deviceName, resourceName);
    markButton(doorOpenButton, isDoor && !SensorBindings_inverted(SENSOR_SLOT_DOOR));
    markButton(doorClosedButton, isDoor && SensorBindings_inverted(SENSOR_SLOT_DOOR));
    markButton(restButton, SensorBindings_matches(SENSOR_SLOT_REST, deviceName, resourceName));
    markButton(acButton, Targets_isAc(deviceName));

    if (job)
    {
        lv_obj_clear_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(clearButton, LV_OBJ_FLAG_HIDDEN);
    }
}
