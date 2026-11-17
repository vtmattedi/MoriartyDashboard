#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"
#include "Pager.h"

#include "App/Registry.h"
#include "App/Targets.h"
#include "App/Net.h"

#include <boardstuff.h>

// Everything the panel has heard announce itself, with its online state and how
// long ago it was heard from. Tapping a row opens the device, where its
// resources, the role binding, reboot/delete and the info queries live.
//
// Seven reusable rows, moved through the registry by swiping or the Prev/Next
// buttons -- not one row per device. The list used to build a row for every
// device it could and stop when the heap ran short, which looked exactly like
// devices missing from the network.

namespace
{
    const int RowsPerPage = 7;

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
    lv_obj_t *pagerRow = nullptr;
    lv_obj_t *previousButton = nullptr;
    lv_obj_t *nextButton = nullptr;
    lv_obj_t *pageLabel = nullptr;
    DeviceRow rows[RowsPerPage];
    int firstIndex = 0;

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

    void onRowClicked(lv_event_t *event)
    {
        if (Pager_justSwiped())
        {
            return;
        }
        const int slot = (int)(intptr_t)lv_event_get_user_data(event);
        const DeviceEntry *entry = Registry_device(firstIndex + slot);
        if (!entry)
        {
            return;
        }
        // Copied first: opening the page releases this one's rows, and a row's
        // own click handler is the caller.
        const std::string name = entry->name;
        ScreenDevice_open(name);
        Ui_show(UI_PAGE_DEVICE_DETAIL);
    }

    void previousPage()
    {
        if (firstIndex >= RowsPerPage)
        {
            firstIndex -= RowsPerPage;
            Ui_markDirty();
        }
    }

    void nextPage()
    {
        if (firstIndex + RowsPerPage < Registry_deviceCount())
        {
            firstIndex += RowsPerPage;
            Ui_markDirty();
        }
    }

    void onPreviousClicked(lv_event_t *) { previousPage(); }
    void onNextClicked(lv_event_t *) { nextPage(); }

    /// Build one of the reusable rows if it does not exist yet. False when the
    /// heap is too tight right now; the next refresh retries.
    bool ensureRow(int slot)
    {
        DeviceRow &row = rows[slot];
        if (row.root)
        {
            return true;
        }
        if (!Ui_canAllocateRow())
        {
            return false;
        }

        row.root = lv_obj_create(page);
        // After the pager row and the empty label, in slot order.
        lv_obj_move_to_index(row.root, slot + 2);
        lv_obj_remove_style_all(row.root);
        lv_obj_set_size(row.root, UI_CARD_W, 52);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.root, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(row.root, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row.root, 10, LV_PART_MAIN);
        lv_obj_clear_flag(row.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row.root, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

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
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.name, UI_CARD_W - 20 - 20 - 70);
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
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(page, UI_GUTTER, LV_PART_MAIN);
    lv_obj_set_style_pad_row(page, 4, LV_PART_MAIN);

    // The pager row exists for the life of the UI; only the rows are released
    // and rebuilt as the screen is used.
    pagerRow = Theme_row(page, UI_CARD_W, 34);
    previousButton = Theme_button(pagerRow, LV_SYMBOL_LEFT "  Prev", 88, 32);
    lv_obj_add_event_cb(previousButton, onPreviousClicked, LV_EVENT_CLICKED, nullptr);
    pageLabel = Theme_label(pagerRow, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
    nextButton = Theme_button(pagerRow, "Next  " LV_SYMBOL_RIGHT, 88, 32);
    lv_obj_add_event_cb(nextButton, onNextClicked, LV_EVENT_CLICKED, nullptr);

    emptyLabel = Theme_label(page, "Listening for devices...", &lv_font_montserrat_14, UI_COL_TEXT_DIM);

    static const SwipeHandlers swipe = {nextPage, previousPage};
    Pager_onSwipe(page, &swipe);

    // Rows are built on demand in the refresh below, not here: at this point
    // WiFi has not started, and committing a screenful of widgets now is what
    // starves the network stack later.
    return page;
}

void ScreenDevices_release()
{
    // Safe even when called from a row's own click handler (opening the detail
    // page leaves this one): LVGL 8.3 marks an event whose target is deleted
    // and resets the input device, so nothing touches the row afterwards.
    for (int i = 0; i < RowsPerPage; ++i)
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

    // Keep the page valid if devices disappear (a deleted device).
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
        DeviceRow &row = rows[slot];
        const DeviceEntry *entry = Registry_device(firstIndex + slot);

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

        lv_obj_set_style_bg_color(row.dot, entry->online ? UI_COL_OK : UI_COL_DANGER, LV_PART_MAIN);
        Theme_setText(row.name, entry->name.c_str());

        char age[24];
        Ui_formatAge(entry->lastSeen, age, sizeof(age));
        char detail[48];
        snprintf(detail, sizeof(detail), "%s - %s", entry->online ? "online" : "offline", age);
        Theme_setText(row.detail, detail);

        const int role = Targets_slotOf(entry->name);
        if (role >= 0)
        {
            Theme_chipSet(row.badge, Targets_label((TargetSlot)role), UI_COL_ACCENT);
            lv_obj_clear_flag(row.badge, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(row.badge, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (count == 0)
    {
        Theme_setText(emptyLabel, "Listening for devices...");
        lv_obj_clear_flag(emptyLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pagerRow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(pagerRow, LV_OBJ_FLAG_HIDDEN);
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
    const int last = (firstIndex + RowsPerPage < count) ? firstIndex + RowsPerPage : count;
    snprintf(position, sizeof(position), "%d-%d of %d", firstIndex + 1, last, count);
    Theme_setText(pageLabel, position);
    setButtonEnabled(previousButton, firstIndex > 0);
    setButtonEnabled(nextButton, firstIndex + RowsPerPage < count);
}
