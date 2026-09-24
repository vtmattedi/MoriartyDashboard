#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Registry.h"
#include "../App/SensorBindings.h"
#include "../App/Targets.h"

#include <boardstuff.h>

// Every resource the panel has seen on the network, newest value wins.
//
// Each row is one `<device>/resource/<name>/state` heard on the wildcard
// subscription in App/Net.cpp, showing the codec representation exactly as it
// was published -- "true", "23.5", "cool".
//
// This is also the way in to the panel's chooser: tapping a row opens
// ScreenResource, where that exact resource is given a job. The list itself only
// lists -- it shows the current value, how long ago it arrived, and a badge for
// anything already in use.
//
// The rows are built lazily as values arrive and freed when the page is left,
// so a network that publishes a lot of resources cannot grow the heap here.

namespace
{
    struct ResourceRow
    {
        lv_obj_t *root;
        lv_obj_t *name;
        lv_obj_t *device;
        lv_obj_t *value;
        lv_obj_t *badge;
    };

    lv_obj_t *page = nullptr;
    lv_obj_t *emptyLabel = nullptr;
    ResourceRow rows[REGISTRY_MAX_RESOURCES];

    void onRowClicked(lv_event_t *event)
    {
        const int index = (int)(intptr_t)lv_event_get_user_data(event);
        const ResourceEntry *entry = Registry_resource(index);
        if (!entry)
        {
            return;
        }
        // Copied out now: the registry reorders as values arrive, so the index
        // this row was built with stops meaning this resource the moment the
        // page is left.
        Ui_showResource(entry->device, entry->resource);
    }
    /// The job this resource does, or nullptr. Roles come first: a resource the
    /// panel drives is the more interesting fact about it.
    const char *jobLabel(const String &device, const String &resource)
    {
        if (Targets_matches(TARGET_SLOT_LIGHT, device, resource))
        {
            return Targets_label(TARGET_SLOT_LIGHT);
        }
        if (Targets_matches(TARGET_SLOT_RGB, device, resource))
        {
            return Targets_label(TARGET_SLOT_RGB);
        }
        for (int slot = 0; slot < SENSOR_SLOT_COUNT; ++slot)
        {
            if (SensorBindings_matches((SensorSlot)slot, device, resource))
            {
                return SensorBindings_label((SensorSlot)slot);
            }
        }
        return nullptr;
    }

    /// Build row `index` if it does not exist yet. Returns false when the heap is
    /// too tight to spend on it; a later refresh retries.
    bool ensureRow(int index)
    {
        ResourceRow &row = rows[index];
        if (row.root)
        {
            return true;
        }
        if (!Ui_canAllocateRow())
        {
            return false;
        }

        row.root = lv_obj_create(page);
        lv_obj_remove_style_all(row.root);
        lv_obj_set_size(row.root, UI_CARD_W, 46);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row.root, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row.root, 10, LV_PART_MAIN);
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row.root, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

        // Most of the row goes to the names; the value on the right is short.
        const lv_coord_t nameWidth = UI_CARD_W - 190;

        row.name = Theme_label(row.root, "", &lv_font_montserrat_14, UI_COL_TEXT);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.name, nameWidth);
        lv_obj_align(row.name, LV_ALIGN_TOP_LEFT, 0, 5);

        row.device = Theme_label(row.root, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_label_set_long_mode(row.device, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.device, nameWidth);
        lv_obj_align(row.device, LV_ALIGN_BOTTOM_LEFT, 0, -5);

        row.value = Theme_label(row.root, "", &lv_font_montserrat_16, UI_COL_ACCENT);
        lv_obj_align(row.value, LV_ALIGN_RIGHT_MID, 0, 0);

        // Marks a resource that has been given a job.
        row.badge = Theme_chip(row.root);
        lv_obj_align(row.badge, LV_ALIGN_RIGHT_MID, -90, 0);
        lv_obj_add_flag(row.badge, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
}

lv_obj_t *ScreenSensors_create(lv_obj_t *parent)
{
    page = lv_obj_create(parent);
    Theme_plainContainer(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 6, LV_PART_MAIN);

    emptyLabel = Theme_label(page, "No resources heard yet.", &lv_font_montserrat_14, UI_COL_TEXT_DIM);

    // Rows are built on demand in the refresh below, not here -- see the note
    // in ScreenDevices. Binding moved to its own page, which like every other
    // page is built once at boot, while the heap is plentiful.

    return page;
}

void ScreenSensors_release()
{
    // A full list is a lot of LVGL objects, and this page is off-screen almost
    // all the time. Freeing the rows whenever it is left puts that memory back
    // for mbedTLS instead of holding it all night under the resting screen.
    for (int i = 0; i < REGISTRY_MAX_RESOURCES; ++i)
    {
        if (rows[i].root)
        {
            lv_obj_del(rows[i].root);
            rows[i] = ResourceRow();
        }
    }
}

void ScreenSensors_refresh()
{
    const int count = Registry_resourceCount();
    int shown = 0;

    for (int i = 0; i < REGISTRY_MAX_RESOURCES; ++i)
    {
        ResourceRow &row = rows[i];
        const ResourceEntry *entry = Registry_resource(i);

        if (!entry)
        {
            if (row.root)
            {
                lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (!ensureRow(i))
        {
            continue;
        }
        ++shown;
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_HIDDEN);

        // Only rewritten when changed: this refresh runs several times a second
        // on a busy broker, and every lv_label_set_text is a heap realloc plus a
        // redraw even when the text is identical.
        Theme_setText(row.name, entry->resource.c_str());

        char age[24];
        Ui_formatAge(entry->lastSeen, age, sizeof(age));
        char detail[80];
        snprintf(detail, sizeof(detail), "%s - %s", entry->device.c_str(), age);
        Theme_setText(row.device, detail);

        // A JSON document has no business in a 90px column -- it would be
        // clipped to something misleading like `{"state":1,"tar`. The type is
        // the honest summary, and the whole document is one tap away on the
        // resource page. Dimmed, so it reads as "what this is" rather than as
        // a value that happens to spell "struct".
        const bool isStruct = Ui_valueIsStruct(entry->value);
        Theme_setText(row.value, isStruct ? "struct" : entry->value.c_str());
        lv_obj_set_style_text_color(row.value, isStruct ? UI_COL_TEXT_FAINT : UI_COL_ACCENT,
                                    LV_PART_MAIN);

        const char *job = jobLabel(entry->device, entry->resource);
        if (job)
        {
            Theme_chipSet(row.badge, job, UI_COL_COOL);
            lv_obj_clear_flag(row.badge, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(row.badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // The label that says "nothing yet" doubles as the one that says "not all
    // of it". A row costs about a kilobyte of LVGL objects and Ui_canAllocateRow()
    // refuses to spend it below the floor, so on a panel this tight the list
    // stops short -- and it used to stop short in silence, which reads as the
    // network having fewer resources than it does. The label is built once with the
    // page, so reporting the shortfall costs nothing at the moment there is
    // nothing to spare.
    if (count == 0)
    {
        Theme_setText(emptyLabel, "No resources heard yet.");
        lv_obj_clear_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else if (shown < count)
    {
        char note[64];
        snprintf(note, sizeof(note), "%d of %d shown - not enough memory for the rest", shown,
                 count);
        Theme_setText(emptyLabel, note);
        lv_obj_clear_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
}
