#include "Ui.h"
#include "Screens.h"
#include "Theme.h"

#include "../App/Net.h"
#include "../App/Registry.h"
#include "../App/Forecast.h"
#include "../App/Targets.h"
#include "../App/Mqttp.h"
#include "../App/SensorBindings.h"
#include "../App/Heap.h"

#include <boardstuff.h>
#include <NightMare.h>

namespace
{
    lv_obj_t *pages[UI_PAGE_COUNT] = {nullptr};
    lv_obj_t *navButtons[UI_PAGE_COUNT] = {nullptr};
    lv_obj_t *restOverlay = nullptr;

    UiPage currentPage = UI_PAGE_FOCUS;
    bool resting = false;
    bool dirty = true;

    uint32_t lastActivityMs = 0;
    uint32_t lastRefreshMs = 0;
    uint32_t lastRevisions = 0;

    // When rest began, so waking can mean "a touch arrived after resting".
    uint32_t restEnteredMs = 0;
    // Touches this soon after resting do not count as a wake. Covers the tail
    // of the very press that asked to rest (Rest now) -- a controller can
    // report a reading or two as the finger lifts.
    const uint32_t RestWakeGraceMs = 400;

    // Read once at startup so a panel can be tuned per room without a reflash.
    uint32_t restAfterMs = 60000;
    uint8_t backlightActive = 200;
    uint8_t backlightRest = 24;

    // Refresh rate for label updates. Fast enough that a tap feels answered,
    // slow enough that redrawing text is not what the CPU spends its day on.
    const uint32_t RefreshIntervalMs = 250;

    void onNavClicked(lv_event_t *event)
    {
        const UiPage page = (UiPage)(intptr_t)lv_event_get_user_data(event);
        Ui_show(page);
    }

    void styleNavButton(lv_obj_t *button, bool selected)
    {
        lv_obj_set_style_text_color(button, selected ? UI_COL_ACCENT : UI_COL_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(button, selected ? LV_OPA_10 : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(button, UI_COL_ACCENT, LV_PART_MAIN);
    }

    lv_obj_t *buildNavBar(lv_obj_t *parent)
    {
        // One line each: the bar is only 46px deep, so icon and text sit side by
        // side rather than stacked.
        static const char *labels[UI_NAV_TAB_COUNT] = {
            LV_SYMBOL_HOME "  Home",
            LV_SYMBOL_WIFI "  Devices",
            LV_SYMBOL_LIST "  Resources",
            LV_SYMBOL_SETTINGS "  Setup",
        };

        lv_obj_t *bar = lv_obj_create(parent);
        Theme_plainContainer(bar);
        lv_obj_set_size(bar, SCREEN_WIDTH, UI_NAVBAR_H);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
        lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (int i = 0; i < UI_NAV_TAB_COUNT; ++i)
        {
            lv_obj_t *button = lv_btn_create(bar);
            lv_obj_remove_style_all(button);
            lv_obj_set_size(button, (SCREEN_WIDTH / UI_NAV_TAB_COUNT) - 10, UI_NAVBAR_H - 8);
            lv_obj_set_style_radius(button, 10, LV_PART_MAIN);
            lv_obj_add_event_cb(button, onNavClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *label = lv_label_create(button);
            lv_label_set_text(label, labels[i]);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_center(label);

            navButtons[i] = button;
            styleNavButton(button, i == 0);
        }
        return bar;
    }

    /// One number that changes whenever anything a screen renders has changed,
    /// so the refresh pass can skip the work when the panel is idle.
    uint32_t dataRevision()
    {
        return Registry_revision() * 31u + Forecast_revision() * 7u + Targets_revision() +
               Mqttp_revision() * 131u + SensorBindings_revision() * 17u;
    }

    /// Hand back what a page only needs while it is visible.
    void releasePage(UiPage page)
    {
        switch (page)
        {
        case UI_PAGE_FOCUS:
            // Home comes back to its overview, not a half-finished adjustment.
            ScreenFocus_collapse();
            break;
        case UI_PAGE_DEVICES:
            ScreenDevices_release();
            break;
        case UI_PAGE_SENSORS:
            ScreenSensors_release();
            break;
        case UI_PAGE_DEVICE_DETAIL:
            ScreenDevice_release();
            break;
        case UI_PAGE_RESOURCE_DETAIL:
            ScreenResource_release();
            break;
        default:
            break;
        }
    }

    void enterRest()
    {
        if (resting)
        {
            return;
        }
        resting = true;
        restEnteredMs = millis();
        // The page stays current under the overlay, but nobody can see it, and
        // the panel may rest for hours. Its rows are rebuilt on wake.
        releasePage(currentPage);
        ScreenRest_refresh();
        // The colour picker and the bind dialog are built lazily onto the same
        // top layer, so by then they sit above this overlay. Raise it, or a
        // dialog left open would show through the resting screen.
        lv_obj_move_foreground(restOverlay);
        lv_obj_clear_flag(restOverlay, LV_OBJ_FLAG_HIDDEN);
        set_backlight(backlightRest);
    }

    void leaveRest()
    {
        if (!resting)
        {
            return;
        }
        resting = false;
        lv_obj_add_flag(restOverlay, LV_OBJ_FLAG_HIDDEN);
        set_backlight(backlightActive);
        // Rebuild whatever enterRest() released, without waiting for data to
        // change first.
        dirty = true;
    }

    void onTouchDetected()
    {
        // Called from the LVGL input driver, so it must stay trivial.
        lastActivityMs = millis();
    }
}

bool Ui_canAllocateRow()
{
    // A row is a handful of LVGL objects, on the order of a kilobyte. These
    // floors keep the list from being the allocation that fails: LVGL 8 asserts
    // rather than returns on a failed malloc, and lib/lv_conf.h turns that
    // assert into a restart.
    //
    // The previous floors -- 40KB free, 18KB largest -- were written against
    // ESP.getFreeHeap()/getMaxAllocHeap(), which include IRAM this cannot use.
    // getMaxAllocHeap() reads a fixed 36852 on this board, so the second test
    // was always true and the guard never refused anything. See App/Heap.h.
    //
    // PROVISIONAL. The old values also aimed to keep an MQTT reconnect
    // possible, which these cannot: mbedTLS needs two 16KB contiguous record
    // buffers and the real heap while connected has ~3KB in its largest block,
    // so no row policy makes a reconnect fit. That is a shortage to fix
    // elsewhere, not a threshold to tune. These are set to the narrower goal --
    // do not be the allocation that reboots the panel -- and want revisiting
    // once the shortage is dealt with.
    const size_t MinFreeHeapForRow = 6 * 1024;
    const size_t MinLargestBlockForRow = 2 * 1024;
    return Heap_free() > MinFreeHeapForRow && Heap_largest() > MinLargestBlockForRow;
}

bool Ui_valueIsStruct(const String &value)
{
    // Cheaper than a parse, and a parse would be the wrong test anyway: a
    // payload that opens a document but is malformed is still not something to
    // print in a 90px column. Whether it parses is the detail page's problem.
    for (size_t i = 0; i < value.length(); ++i)
    {
        const char c = value[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }
        return c == '{' || c == '[';
    }
    return false;
}

void Ui_formatAge(uint32_t stamp, char *out, size_t outSize)
{
    const uint32_t current = (uint32_t)NightMare::Time::now();
    if (!stamp || current < stamp)
    {
        snprintf(out, outSize, "just now");
        return;
    }

    // No seconds: text that changes every second means every visible row is
    // rewritten -- a realloc and a redraw each -- every second. Minutes are
    // plenty for "is this device still talking".
    const uint32_t age = current - stamp;
    if (age < 10)
    {
        snprintf(out, outSize, "just now");
    }
    else if (age < 60)
    {
        snprintf(out, outSize, "<1m ago");
    }
    else if (age < 3600)
    {
        snprintf(out, outSize, "%lum ago", (unsigned long)(age / 60));
    }
    else if (age < 86400)
    {
        snprintf(out, outSize, "%luh ago", (unsigned long)(age / 3600));
    }
    else
    {
        snprintf(out, outSize, "%lud ago", (unsigned long)(age / 86400));
    }
}

void Ui_markDirty()
{
    dirty = true;
}

void Ui_reloadSettings()
{
    // Cached rather than read from the settings store on every tick: its get()
    // is a linear string search, and Ui_tick runs every pass of loop().
    restAfterMs = (uint32_t)PersistentSettings.get("rest_after", "60").toInt() * 1000UL;
    backlightActive = (uint8_t)constrain(PersistentSettings.get("bl_active", "200").toInt(), 8, 255);
    backlightRest = (uint8_t)constrain(PersistentSettings.get("bl_rest", "24").toInt(), 0, 255);

    // A changed timeout counts from now, not from the last touch -- otherwise
    // shortening it could drop the panel into rest the instant the finger lifts.
    lastActivityMs = millis();
}

void Ui_wake()
{
    lastActivityMs = millis();
    leaveRest();
}

bool Ui_resting()
{
    return resting;
}

void Ui_restNow()
{
    enterRest();
}

void Ui_show(UiPage page)
{
    if (page < 0 || page >= UI_PAGE_COUNT)
    {
        return;
    }

    if (page != currentPage)
    {
        releasePage(currentPage);
    }

    currentPage = page;
    for (int i = 0; i < UI_PAGE_COUNT; ++i)
    {
        if (i == page)
        {
            lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // The detail pages have no tab of their own; each keeps lit the list it was
    // opened from, which is also where Back returns to.
    int litTab = page;
    if (page == UI_PAGE_DEVICE_DETAIL)
    {
        litTab = UI_PAGE_DEVICES;
    }
    else if (page == UI_PAGE_RESOURCE_DETAIL)
    {
        litTab = UI_PAGE_SENSORS;
    }
    for (int i = 0; i < UI_NAV_TAB_COUNT; ++i)
    {
        styleNavButton(navButtons[i], i == litTab);
    }
    dirty = true;
}

void Ui_showDevice(const String &deviceName)
{
    ScreenDevice_open(deviceName);
    Ui_show(UI_PAGE_DEVICE_DETAIL);
}

void Ui_showResource(const String &deviceName, const String &resourceName)
{
    ScreenResource_open(deviceName, resourceName);
    Ui_show(UI_PAGE_RESOURCE_DETAIL);
}

void Ui_begin()
{
    Theme_init();

    Ui_reloadSettings();

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    StatusBar_create(screen);
    lv_obj_t *nav = buildNavBar(screen);

    // The pages share one container so switching is a hide/show, not a rebuild.
    lv_obj_t *content = lv_obj_create(screen);
    Theme_plainContainer(content);
    lv_obj_set_size(content, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, UI_STATUSBAR_H);
    lv_obj_move_background(content);
    lv_obj_move_foreground(nav);

    // TEMPORARY SCAFFOLDING. Building every screen at boot costs this panel
    // about 55KB, which it then holds for good -- and it has roughly 4KB left
    // once it has joined the network, so that 55KB is the difference between a
    // panel that can re-establish a TLS session and one that cannot. Before
    // cutting anything, find out which screens the 55KB is actually in: two of
    // them being most of it is a very different fix from six being equal.
    // Remove these probes once that is known.
    Heap_log("ui: chrome");
    pages[UI_PAGE_FOCUS] = ScreenFocus_create(content);
    Heap_log("ui: focus");
    pages[UI_PAGE_DEVICES] = ScreenDevices_create(content);
    Heap_log("ui: devices");
    pages[UI_PAGE_SENSORS] = ScreenSensors_create(content);
    Heap_log("ui: resources");
    pages[UI_PAGE_SETTINGS] = ScreenSettings_create(content);
    Heap_log("ui: settings");
    pages[UI_PAGE_DEVICE_DETAIL] = ScreenDevice_create(content);
    Heap_log("ui: device");
    pages[UI_PAGE_RESOURCE_DETAIL] = ScreenResource_create(content);
    Heap_log("ui: resource");

    // The resting clock sits on the top layer so it covers the nav bar and the
    // status bar as well as the page.
    restOverlay = ScreenRest_create(lv_layer_top());
    lv_obj_add_flag(restOverlay, LV_OBJ_FLAG_HIDDEN);
    Heap_log("ui: rest");

    touch_callback = onTouchDetected;
    lastActivityMs = millis();
    Ui_show(UI_PAGE_FOCUS);
    set_backlight(backlightActive);
}

void Ui_tick()
{
    const uint32_t nowMs = millis();

    // Wake on a touch that arrived after rest began. Not "a touch within the
    // idle timeout", which is what this used to test: that woke the panel
    // straight back up after Rest now (the tap itself is recent), and with the
    // timeout set to never it could never wake at all.
    //
    // Any touch while resting is spent on waking up -- the overlay absorbs it --
    // so a stray tap on the dark screen cannot also press what is underneath.
    // Signed difference so this holds across the millis() rollover.
    if (resting && (int32_t)(lastActivityMs - restEnteredMs) > (int32_t)RestWakeGraceMs)
    {
        leaveRest();
    }
    else if (!resting && restAfterMs > 0 && (nowMs - lastActivityMs) >= restAfterMs)
    {
        enterRest();
    }

    if ((nowMs - lastRefreshMs) < RefreshIntervalMs)
    {
        return;
    }
    lastRefreshMs = nowMs;

    const uint32_t revision = dataRevision();
    if (revision != lastRevisions)
    {
        lastRevisions = revision;
        dirty = true;
    }

    if (resting)
    {
        ScreenRest_refresh(); // The clock has to keep ticking.
        return;
    }

    // The status bar carries the clock and link state, so it always refreshes.
    StatusBar_refresh();

    if (!dirty)
    {
        // These two still refresh: the focus screen's values are read straight
        // out of the bound resources, whose optimistic writes and expiries
        // belong to no revision, and the settings screen shows live heap and
        // uptime that nothing bumps a revision for.
        if (currentPage == UI_PAGE_FOCUS)
        {
            ScreenFocus_refresh();
        }
        else if (currentPage == UI_PAGE_SETTINGS)
        {
            ScreenSettings_refresh();
        }
        return;
    }
    dirty = false;

    switch (currentPage)
    {
    case UI_PAGE_FOCUS:
        ScreenFocus_refresh();
        break;
    case UI_PAGE_DEVICES:
        ScreenDevices_refresh();
        break;
    case UI_PAGE_SENSORS:
        ScreenSensors_refresh();
        break;
    case UI_PAGE_SETTINGS:
        ScreenSettings_refresh();
        break;
    case UI_PAGE_DEVICE_DETAIL:
        ScreenDevice_refresh();
        break;
    case UI_PAGE_RESOURCE_DETAIL:
        ScreenResource_refresh();
        break;
    default:
        break;
    }
}
