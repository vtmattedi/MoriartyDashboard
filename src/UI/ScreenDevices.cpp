#include "Screens.h"
#include "Theme.h"
#include "Ui.h"

#include "../App/Registry.h"
#include "../App/Targets.h"
#include "../App/Net.h"

#include <boardstuff.h>

// Everything the panel has heard announce itself, with its online state and how
// long ago it was heard from. Tapping a row opens the detail page, where the
// role binding, reboot/delete and the info queries live.

namespace
{
    struct DeviceRow
    {
        lv_obj_t *root;
        lv_obj_t *dot;
        lv_obj_t *name;
        lv_obj_t *detail;
        lv_obj_t *badge;
    };

    lv_obj_t *page = nullptr;
    lv_obj_t *emptyLabel = nullptr;
    DeviceRow rows[REGISTRY_MAX_DEVICES];

    void onRowClicked(lv_event_t *event)
    {
        const int index = (int)(intptr_t)lv_event_get_user_data(event);
        const DeviceEntry *entry = Registry_device(index);
        if (!entry)
        {
            return;
        }

        ScreenDevice_open(entry->name);
        Ui_show(UI_PAGE_DEVICE_DETAIL);
    }

    /// Build row `index` if it does not exist yet. Returns false when it could
    /// not be built, either because it already failed or because the heap is
    /// too tight to spend on it right now -- a later refresh will retry.
    bool ensureRow(int index)
    {
        DeviceRow &row = rows[index];
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
        lv_obj_set_size(row.root, UI_CARD_W, 52);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row.root, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row.root, 10, LV_PART_MAIN);
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row.root, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

        row.dot = lv_obj_create(row.root);
        lv_obj_remove_style_all(row.dot);
        // Purely an indicator. lv_obj_create makes it clickable by default,
        // which would swallow a tap on it instead of opening the row.
        lv_obj_clear_flag(row.dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(row.dot, 10, 10);
        lv_obj_set_style_radius(row.dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(row.dot, LV_ALIGN_LEFT_MID, 0, 0);

        row.name = Theme_label(row.root, "", &lv_font_montserrat_16, UI_COL_TEXT);
        lv_obj_align(row.name, LV_ALIGN_TOP_LEFT, 20, 7);

        row.detail = Theme_label(row.root, "", &lv_font_montserrat_14, UI_COL_TEXT_FAINT);
        lv_obj_align(row.detail, LV_ALIGN_BOTTOM_LEFT, 20, -7);

        row.badge = Theme_chip(row.root);
        lv_obj_align(row.badge, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
}

lv_obj_t *ScreenDevices_create(lv_obj_t *parent)
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

    emptyLabel = Theme_label(page, "Listening for devices...", &lv_font_montserrat_14, UI_COL_TEXT_DIM);

    // Rows are built on demand in the refresh below, not here: at this point
    // WiFi has not started and the TLS MQTT client has not claimed its memory,
    // so committing a full screen of widgets now is what starves it later.

    return page;
}

void ScreenDevices_release()
{
    // Safe even when called from a row's own click handler (opening the detail
    // page leaves this one): LVGL 8.3 marks an event whose target is deleted
    // and resets the input device, so nothing touches the row afterwards.
    for (int i = 0; i < REGISTRY_MAX_DEVICES; ++i)
    {
        if (rows[i].root)
        {
            lv_obj_del(rows[i].root);
            rows[i] = DeviceRow();
        }
    }
}

void ScreenDevices_refresh()
{
    const int count = Registry_deviceCount();
    int shown = 0;

    for (int i = 0; i < REGISTRY_MAX_DEVICES; ++i)
    {
        DeviceRow &row = rows[i];
        const DeviceEntry *entry = Registry_device(i);

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

        lv_obj_set_style_bg_color(row.dot, entry->online ? UI_COL_OK : UI_COL_DANGER, LV_PART_MAIN);
        Theme_setText(row.name, entry->name.c_str());

        char age[24];
        Ui_formatAge(entry->lastSeen, age, sizeof(age));
        char detail[48];
        snprintf(detail, sizeof(detail), "%s - %s", entry->online ? "online" : "offline", age);
        Theme_setText(row.detail, detail);

        const int slot = Targets_slotOf(entry->name);
        if (slot >= 0)
        {
            Theme_chipSet(row.badge, Targets_label((TargetSlot)slot), UI_COL_ACCENT);
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
    // network having fewer devices than it does. The label is built once with the
    // page, so reporting the shortfall costs nothing at the moment there is
    // nothing to spare.
    if (count == 0)
    {
        Theme_setText(emptyLabel, "Listening for devices...");
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
