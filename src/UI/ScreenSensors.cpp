#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Registry.h"
#include "../App/SensorBindings.h"
#include "../App/Targets.h"

#include <boardstuff.h>

// Every resource the panel has seen on the network, newest value wins.
//
// Four reusable rows are kept on screen at once. The registry can hold forty
// resources, but allocating a set of LVGL objects for every one needlessly
// competes with TLS for the ESP32's small heap. Previous/next buttons move the
// same four widgets through the registry instead.

namespace
{
    const int RowsPerPage = 4;

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
    lv_obj_t *pager = nullptr;
    lv_obj_t *previousButton = nullptr;
    lv_obj_t *nextButton = nullptr;
    lv_obj_t *pageLabel = nullptr;
    ResourceRow rows[RowsPerPage];
    int firstIndex = 0;

    void onRowClicked(lv_event_t *event)
    {
        const int slot = (int)(intptr_t)lv_event_get_user_data(event);
        const ResourceEntry *entry = Registry_resource(firstIndex + slot);
        if (entry)
        {
            // Copy the address before changing pages; registry entries remain
            // live and may be replaced when a full table evicts an old value.
            Ui_showResource(entry->device, entry->resource);
        }
    }

    void onPreviousClicked(lv_event_t *)
    {
        if (firstIndex >= RowsPerPage)
        {
            firstIndex -= RowsPerPage;
            Ui_markDirty();
        }
    }

    void onNextClicked(lv_event_t *)
    {
        if (firstIndex + RowsPerPage < Registry_resourceCount())
        {
            firstIndex += RowsPerPage;
            Ui_markDirty();
        }
    }

    void setButtonEnabled(lv_obj_t *button, bool enabled)
    {
        if (enabled)
        {
            lv_obj_clear_state(button, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(button, LV_STATE_DISABLED);
        }
    }

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

    /// Build one of the four reusable rows if it does not exist yet. Returns
    /// false when the heap is temporarily too tight; a later refresh retries.
    bool ensureRow(int slot)
    {
        ResourceRow &row = rows[slot];
        if (row.root)
        {
            return true;
        }
        if (!Ui_canAllocateRow())
        {
            return false;
        }

        row.root = lv_obj_create(page);
        // Rows are created after the persistent pager and empty-state label.
        // Keep the pager first, then place rows in their slot order.
        lv_obj_move_to_index(row.root, slot + 1);
        lv_obj_remove_style_all(row.root);
        lv_obj_set_size(row.root, UI_CARD_W, 44);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row.root, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row.root, 10, LV_PART_MAIN);
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row.root, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

        const lv_coord_t nameWidth = UI_CARD_W - 190;

        row.name = Theme_label(row.root, "", &lv_font_montserrat_14, UI_COL_TEXT);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.name, nameWidth);
        lv_obj_align(row.name, LV_ALIGN_TOP_LEFT, 0, 4);

        row.device = Theme_label(row.root, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_label_set_long_mode(row.device, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.device, nameWidth);
        lv_obj_align(row.device, LV_ALIGN_BOTTOM_LEFT, 0, -4);

        row.value = Theme_label(row.root, "", &lv_font_montserrat_16, UI_COL_ACCENT);
        lv_obj_align(row.value, LV_ALIGN_RIGHT_MID, 0, 0);

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
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 4, LV_PART_MAIN);

    // The pager exists for the lifetime of the UI. Only the four comparatively
    // expensive resource rows are released and rebuilt as the screen is used.
    pager = Theme_row(page, UI_CARD_W, 34);
    previousButton = Theme_button(pager, LV_SYMBOL_LEFT "  Previous", 112, 32);
    lv_obj_add_event_cb(previousButton, onPreviousClicked, LV_EVENT_CLICKED, nullptr);

    pageLabel = Theme_label(pager, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);

    nextButton = Theme_button(pager, "Next  " LV_SYMBOL_RIGHT, 112, 32);
    lv_obj_add_event_cb(nextButton, onNextClicked, LV_EVENT_CLICKED, nullptr);

    emptyLabel = Theme_label(page, "No resources heard yet.", &lv_font_montserrat_14,
                             UI_COL_TEXT_DIM);

    return page;
}

void ScreenSensors_release()
{
    for (int i = 0; i < RowsPerPage; ++i)
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

    // Keep the current page valid if resources disappear with a deleted device.
    if (count == 0)
    {
        firstIndex = 0;
    }
    else if (firstIndex >= count)
    {
        firstIndex = ((count - 1) / RowsPerPage) * RowsPerPage;
    }

    int shown = 0;
    for (int slot = 0; slot < RowsPerPage; ++slot)
    {
        ResourceRow &row = rows[slot];
        const ResourceEntry *entry = Registry_resource(firstIndex + slot);
        if (!entry)
        {
            if (row.root)
            {
                lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (!ensureRow(slot))
        {
            continue;
        }

        ++shown;
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_HIDDEN);
        Theme_setText(row.name, entry->resource.c_str());

        char age[24];
        Ui_formatAge(entry->lastSeen, age, sizeof(age));
        char detail[80];
        snprintf(detail, sizeof(detail), "%s - %s", entry->device.c_str(), age);
        Theme_setText(row.device, detail);

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

    if (count == 0)
    {
        Theme_setText(emptyLabel, "No resources heard yet.");
        lv_obj_clear_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pager, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(pager, LV_OBJ_FLAG_HIDDEN);
    const int expected = (count - firstIndex < RowsPerPage) ? count - firstIndex : RowsPerPage;
    if (shown < expected)
    {
        Theme_setText(emptyLabel, "Not enough memory to draw this page yet.");
        lv_obj_clear_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
    }

    char position[24];
    const int lastIndex = (firstIndex + RowsPerPage < count) ? firstIndex + RowsPerPage : count;
    snprintf(position, sizeof(position), "%d-%d of %d", firstIndex + 1, lastIndex, count);
    Theme_setText(pageLabel, position);
    setButtonEnabled(previousButton, firstIndex > 0);
    setButtonEnabled(nextButton, firstIndex + RowsPerPage < count);
}
