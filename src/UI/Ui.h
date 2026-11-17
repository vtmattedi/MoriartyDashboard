#pragma once
#include <stdint.h>
#include <string>

#include "Sys.h"
#include <lvgl.h>

// The panel's screens and the manager that switches between them.
//
// The manager also owns the three power stages, because what moves the panel
// between them is the same thing it already tracks -- how long since a touch:
//
//   active    in use. Full backlight, everything live.
//   resting   idle for `rest_after`. The resting clock at a dim backlight; the
//             network stays up, so the clock's corners stay true and a touch
//             brings the page back instantly.
//   asleep    on the resting screen for `sleep_after`. Radio off, display off,
//             processor in light sleep. A touch wakes it into `active`, and the
//             network rejoins in the background over the next few seconds.

enum UiPage
{
    /// Home: a grid of tiles, each a control that shows its own state or a door
    /// to another page. Reached by waking from rest, and by Back from anywhere.
    UI_PAGE_MENU = 0,
    /// The full controls for the AC, light, RGB and PC (the old home page).
    /// Opened by holding a tile on the menu.
    UI_PAGE_FOCUS,
    UI_PAGE_DEVICES,
    UI_PAGE_SENSORS,
    UI_PAGE_SETTINGS,
    /// Pushed from a row on a list rather than reached from the menu. Back
    /// returns to the list it was opened from.
    UI_PAGE_DEVICE_DETAIL,
    UI_PAGE_RESOURCE_DETAIL,
    UI_PAGE_COUNT
};

/// @brief Build every screen. Call once, after lvgl_begin() and
/// introNightMareESP(), which is what opens the settings store.
void Ui_begin();

/// @brief Drive refreshes and the idle-to-rest transition. Call from loop().
void Ui_tick();

/// @brief Go back one level: from a detail page to its list, from a list or
/// settings to the menu. Does nothing on the menu.
void Ui_back();

/// @brief Switch the visible page.
void Ui_show(UiPage page);

/// @brief Open the detail page for one device.
void Ui_showDevice(const std::string &deviceName);

/// @brief Open the detail page for one resource, where it is given its job.
void Ui_showResource(const std::string &deviceName, const std::string &resourceName);

/// @brief Count user activity: leaves rest mode and restarts the idle timer.
void Ui_wake();

/// @brief True while the resting clock is showing.
bool Ui_resting();

/// @brief Go to the resting screen immediately, regardless of the idle timeout.
/// The next touch wakes it, even with the timeout set to never.
void Ui_restNow();

/// @brief Go to sleep on the next tick, regardless of the timeouts: radio off,
/// display off, processor asleep until the glass is touched.
///
/// Deferred rather than done here, because sleeping blocks until that touch and
/// the callers are a button's event handler and a console command -- neither is
/// a place to stop the world from.
void Ui_sleepNow();

/// @brief Request a redraw on the next tick, after underlying data changed.
void Ui_markDirty();

/// @brief Re-read the display settings (bl_active, bl_rest, rest_after,
/// sleep_after) from the settings store. The manager caches them, so anything that changes them
/// must call this or the change only takes effect after a reboot.
void Ui_reloadSettings();
