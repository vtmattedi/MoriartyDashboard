#pragma once
#include <Arduino.h>
#include <lvgl.h>

// The panel's screens and the manager that switches between them.

enum UiPage
{
    UI_PAGE_FOCUS = 0,
    UI_PAGE_DEVICES,
    UI_PAGE_SENSORS,
    UI_PAGE_SETTINGS,
    /// Pushed from a row on a list rather than reached from the nav rail, so
    /// these sit after the tabbed pages and have no tab of their own. Each
    /// keeps the tab it was opened from lit.
    UI_PAGE_DEVICE_DETAIL,
    UI_PAGE_RESOURCE_DETAIL,
    UI_PAGE_COUNT
};

/// How many of the pages above have a tab in the nav rail.
#define UI_NAV_TAB_COUNT 4

/// @brief Build every screen. Call once, after lvgl_begin() and
/// introNightMareESP(), which is what opens the settings store.
void Ui_begin();

/// @brief Drive refreshes and the idle-to-rest transition. Call from loop().
void Ui_tick();

/// @brief Switch the visible page.
void Ui_show(UiPage page);

/// @brief Open the detail page for one device.
void Ui_showDevice(const String &deviceName);

/// @brief Open the detail page for one resource, where it is given its job.
void Ui_showResource(const String &deviceName, const String &resourceName);

/// @brief Count user activity: leaves rest mode and restarts the idle timer.
void Ui_wake();

/// @brief True while the resting clock is showing.
bool Ui_resting();

/// @brief Go to the resting screen immediately, regardless of the idle timeout.
/// The next touch wakes it, even with the timeout set to never.
void Ui_restNow();

/// @brief Request a redraw on the next tick, after underlying data changed.
void Ui_markDirty();

/// @brief Re-read the display settings (bl_active, bl_rest, rest_after) from
/// the settings store. The manager caches them, so anything that changes them
/// must call this or the change only takes effect after a reboot.
void Ui_reloadSettings();
