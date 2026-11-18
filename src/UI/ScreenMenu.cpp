#include "Screens.h"
#include "Sys.h"
#include "Theme.h"
#include "Ui.h"
#include "Pager.h"

#include "App/AcClient.h"
#include "App/Battery.h"
#include "App/LightControl.h"
#include "App/Ota.h"
#include "App/PcClient.h"
#include "App/Registry.h"
#include "App/Targets.h"

#include <boardstuff.h>
#include <NightMare.h>
#include <stdio.h>
#include <string.h>

// The home page: a grid of big tiles, six to a page, swipe for the next.
//
// Every tile is a button that shows its own state, so most of what this panel is
// for is one glance and one tap -- AC on or off, the light, the PC -- with no
// sliders to find. A tile is one of two kinds:
//
//   control   tap does the obvious binary thing (toggle, send), and the tile
//             lights up when it is on. Hold opens the full controls (UI_PAGE_FOCUS)
//             for the finer adjustments.
//   door      tap opens another page.
//
// A swipe moves between pages of tiles. Nothing here scrolls, so nothing has to
// redraw the whole screen every frame; see Pager.h.

namespace
{
    constexpr int TilesPerPage = 6;
    constexpr int Columns = 2;
    constexpr lv_coord_t TileW = (UI_CONTENT_W - 3 * UI_GUTTER) / Columns;
    constexpr lv_coord_t TileH = (UI_CONTENT_H - UI_PAGER_H - 4 * UI_GUTTER) / 3;

    /// Fills the tile's two status lines and says whether it is lit and whether
    /// it does anything right now.
    using StatusFn = void (*)(char *text, size_t size, bool &active, bool &usable);
    using ActionFn = void (*)();
    /// Small text for the tile's top-right corner (a reading that belongs to
    /// what the tile is); empty when there is none.
    using CornerFn = void (*)(char *text, size_t size);

    struct TileSpec
    {
        const char *icon;
        const char *title;
        lv_color_t colour; // the tile's colour when lit
        StatusFn status;
        ActionFn tap;
        ActionFn hold; // may be null
        CornerFn corner; // may be null
    };

    struct TileView
    {
        lv_obj_t *root;
        lv_obj_t *icon;
        lv_obj_t *title;
        lv_obj_t *status;
        lv_obj_t *corner;
        int8_t lastActive; // -1: never styled
        int8_t lastUsable;
    };

    Pager pager;

    // Set by the PC tile, so the tile can say "sent" for a moment after a tap.
    uint32_t pcSentUntilMs = 0;

#pragma region "States"

    void formatDegrees(char *out, size_t size, float value)
    {
        snprintf(out, size, "%.1f" DEG, value);
    }

    void acStatus(char *text, size_t size, bool &active, bool &usable)
    {
        if (!Targets_configured(TARGET_SLOT_AC))
        {
            snprintf(text, size, "Not bound\ntap to choose");
            usable = false;
            return;
        }
        if (AcClient_isStale() || AcClient_state() == AC_UNKNOWN)
        {
            snprintf(text, size, "No data");
            usable = false;
            return;
        }
        active = AcClient_powerOn();
        char room[16] = "";
        float temperature;
        if (AcClient_roomTemperature(temperature))
        {
            formatDegrees(room, sizeof(room), temperature);
        }
        snprintf(text, size, "%s\n%s", active ? "On" : "Off", room);
    }

    void acTap()
    {
        if (!Targets_configured(TARGET_SLOT_AC))
        {
            Ui_show(UI_PAGE_DEVICES);
            return;
        }
        AcClient_togglePower();
    }

    void lightStatus(char *text, size_t size, bool &active, bool &usable)
    {
        switch (LightControl_state())
        {
        case LIGHT_UNBOUND:
            snprintf(text, size, "Not bound\ntap to choose");
            usable = false;
            break;
        case LIGHT_UNKNOWN:
            snprintf(text, size, "No data");
            usable = false;
            break;
        case LIGHT_ON:
            active = true;
            snprintf(text, size, "On");
            break;
        default:
            snprintf(text, size, "Off");
            break;
        }
    }

    void lightTap()
    {
        if (LightControl_state() == LIGHT_UNBOUND)
        {
            Ui_show(UI_PAGE_DEVICES);
            return;
        }
        LightControl_toggle();
    }

    /// The bed light is Sherlock, which also has a thermometer: its temperature
    /// is shown on the tile, since the bed is where it is measured.
    void bedCorner(char *text, size_t size)
    {
        text[0] = '\0';
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            return;
        }
        const std::string value = Registry_resourceValue(Targets_device(TARGET_SLOT_RGB), "temperature");
        if (value.length())
        {
            snprintf(text, size, "%.1f" DEG, atof(value.c_str()));
        }
    }

    void rgbStatus(char *text, size_t size, bool &active, bool &usable)
    {
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            snprintf(text, size, "Not bound\ntap to choose");
            usable = false;
            return;
        }
        if (!Rgb_hasState())
        {
            snprintf(text, size, "No data");
            usable = false;
            return;
        }
        active = Rgb_on();
        if (active)
        {
            snprintf(text, size, "On");
        }
        else
        {
            snprintf(text, size, "Off");
        }
    }

    void rgbTap()
    {
        if (!Targets_configured(TARGET_SLOT_RGB))
        {
            Ui_show(UI_PAGE_DEVICES);
            return;
        }
        Rgb_setOn(!Rgb_on());
    }

    void pcStatus(char *text, size_t size, bool &active, bool &usable)
    {
        if (!Targets_configured(TARGET_SLOT_PC))
        {
            snprintf(text, size, "Not bound\ntap to choose");
            usable = false;
            return;
        }
        if (PcClient_link() != PC_LINK_UP)
        {
            snprintf(text, size, "No PC linked");
            usable = false;
            return;
        }
        active = true;
        if ((int32_t)(pcSentUntilMs - millisNow()) > 0)
        {
            snprintf(text, size, "Sent");
        }
        else
        {
            snprintf(text, size, "Linked\nhold: sleep PC");
        }
    }

    void pcTap()
    {
        if (!Targets_configured(TARGET_SLOT_PC))
        {
            Ui_show(UI_PAGE_DEVICES);
            return;
        }
        // Monitors off is the tap: it is undone by moving the mouse. Sleeping
        // the PC is the hold, so it is never one stray touch away.
        if (PcClient_monitorsOff())
        {
            pcSentUntilMs = millisNow() + 2500;
        }
    }

    void pcHold()
    {
        if (PcClient_sleep())
        {
            pcSentUntilMs = millisNow() + 2500;
        }
    }

    void restStatus(char *text, size_t size, bool &, bool &)
    {
        snprintf(text, size, "Dim the screen");
    }

    void sleepStatus(char *text, size_t size, bool &, bool &usable)
    {
        if (Ota_active())
        {
            snprintf(text, size, "Not while updating");
            usable = false;
            return;
        }
        snprintf(text, size, "Radio and screen off\ntouch to wake");
    }

    void devicesStatus(char *text, size_t size, bool &, bool &)
    {
        snprintf(text, size, "%d seen\ntap to open one", Registry_deviceCount());
    }

    void batteryStatus(char *text, size_t size, bool &active, bool &)
    {
        if (!Battery_present())
        {
            snprintf(text, size, "No cell\non USB");
            return;
        }
        active = !Battery_low();
        snprintf(text, size, "%u%%\n%u.%02uV", (unsigned)Battery_percent(),
                 (unsigned)(Battery_millivolts() / 1000), (unsigned)((Battery_millivolts() % 1000) / 10));
    }

    void updateStatus(char *text, size_t size, bool &active, bool &)
    {
        active = Ota_active();
        snprintf(text, size, "%s", active ? "Update mode open" : "From the PC");
    }

    void controlsStatus(char *text, size_t size, bool &, bool &)
    {
        snprintf(text, size, "AC, light, RGB,\nweather");
    }

    void settingsStatus(char *text, size_t size, bool &, bool &)
    {
        snprintf(text, size, "Display, power,\nnetwork");
    }

    void openDevices() { Ui_show(UI_PAGE_DEVICES); }
    void openControls() { Ui_show(UI_PAGE_FOCUS); }
    void openSettings() { Ui_show(UI_PAGE_SETTINGS); }
    void openUpdate() { ScreenSettings_open(SETTINGS_PAGE_UPDATE); }
    void openBattery() { ScreenSettings_open(SETTINGS_PAGE_POWER); }
    void restNow() { Ui_restNow(); }
    void sleepNow() { Ui_sleepNow(); }

#pragma endregion

    const TileSpec Tiles[] = {
        // Page 1: the things you reach for, and how the panel is doing.
        {LV_SYMBOL_CHARGE, "Light", UI_COL_WARN, lightStatus, lightTap, openControls},
        {LV_SYMBOL_REFRESH, "AC", UI_COL_COOL, acStatus, acTap, openControls},
        {LV_SYMBOL_TINT, "Bed", UI_COL_ACCENT, rgbStatus, rgbTap, openControls, bedCorner},
        {LV_SYMBOL_KEYBOARD, "PC", UI_COL_COOL, pcStatus, pcTap, pcHold},
        {LV_SYMBOL_BATTERY_3, "Battery", UI_COL_OK, batteryStatus, openBattery, nullptr},
        {LV_SYMBOL_DIRECTORY, "Settings", UI_COL_ACCENT, settingsStatus, openSettings, nullptr},
        // Page 2: power stages, and where everything else is.
        {LV_SYMBOL_EYE_CLOSE, "Rest", UI_COL_ACCENT, restStatus, restNow, nullptr},
        {LV_SYMBOL_POWER, "Sleep", UI_COL_DANGER, sleepStatus, sleepNow, nullptr},
        {LV_SYMBOL_SETTINGS, "Controls", UI_COL_ACCENT, controlsStatus, openControls, nullptr},
        {LV_SYMBOL_WIFI, "Devices", UI_COL_ACCENT, devicesStatus, openDevices, nullptr},
        {LV_SYMBOL_UPLOAD, "Update", UI_COL_ACCENT, updateStatus, openUpdate, nullptr},
    };
    constexpr int TileCount = sizeof(Tiles) / sizeof(Tiles[0]);
    constexpr int PageCount = (TileCount + TilesPerPage - 1) / TilesPerPage;

    TileView views[TileCount];

    void onTileClicked(lv_event_t *event)
    {
        // The finger lifting at the end of a swipe is not a tap.
        if (Pager_justSwiped())
        {
            return;
        }
        const TileSpec &tile = Tiles[(int)(intptr_t)lv_event_get_user_data(event)];
        tile.tap();
        // Show the answer now instead of at the next refresh; optimistic writes
        // have already changed what the status functions read.
        ScreenMenu_refresh();
    }

    void onTileHeld(lv_event_t *event)
    {
        const TileSpec &tile = Tiles[(int)(intptr_t)lv_event_get_user_data(event)];
        if (tile.hold)
        {
            tile.hold();
            ScreenMenu_refresh();
        }
    }

    void buildTile(int index)
    {
        const TileSpec &spec = Tiles[index];
        TileView &view = views[index];
        const int onPage = index % TilesPerPage;
        lv_obj_t *page = pager.page[index / TilesPerPage];

        view.root = lv_obj_create(page);
        lv_obj_remove_style_all(view.root);
        lv_obj_set_size(view.root, TileW, TileH);
        lv_obj_set_pos(view.root, UI_GUTTER + (onPage % Columns) * (TileW + UI_GUTTER),
                       UI_GUTTER + (onPage / Columns) * (TileH + UI_GUTTER));
        lv_obj_clear_flag(view.root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(view.root, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(view.root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(view.root, UI_COL_SURFACE, LV_PART_MAIN);
        lv_obj_set_style_border_width(view.root, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(view.root, UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(view.root, UI_COL_SURFACE_ALT, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_pad_all(view.root, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(view.root, onTileClicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
        lv_obj_add_event_cb(view.root, onTileHeld, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);

        view.icon = Theme_label(view.root, spec.icon, &lv_font_montserrat_28, UI_COL_TEXT_DIM);
        lv_obj_align(view.icon, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(view.icon, LV_OBJ_FLAG_CLICKABLE);

        view.title = Theme_label(view.root, spec.title, &lv_font_montserrat_16, UI_COL_TEXT);
        lv_obj_align(view.title, LV_ALIGN_BOTTOM_LEFT, 0, -36);

        view.corner = Theme_label(view.root, "", &lv_font_montserrat_16, UI_COL_TEXT_DIM);
        lv_obj_align(view.corner, LV_ALIGN_TOP_RIGHT, 0, 4);
        lv_obj_clear_flag(view.corner, LV_OBJ_FLAG_CLICKABLE);

        view.status = Theme_label(view.root, "", &lv_font_montserrat_14, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(view.status, LV_LABEL_LONG_DOT);
        lv_obj_set_size(view.status, TileW - 26, 34);
        lv_obj_align(view.status, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        view.lastActive = -1;
        view.lastUsable = -1;
    }

    void refreshTile(int index)
    {
        const TileSpec &spec = Tiles[index];
        TileView &view = views[index];

        char text[64] = "";
        bool active = false;
        bool usable = true;
        spec.status(text, sizeof(text), active, usable);
        Theme_setText(view.status, text);
        if (spec.corner)
        {
            char corner[16];
            spec.corner(corner, sizeof(corner));
            Theme_setText(view.corner, corner);
        }

        if (view.lastActive == (int8_t)active && view.lastUsable == (int8_t)usable)
        {
            return;
        }
        view.lastActive = (int8_t)active;
        view.lastUsable = (int8_t)usable;

        // A lit tile is its own colour, washed into the surface so the text on
        // it keeps its contrast; a tile that cannot act is dimmed.
        lv_obj_set_style_bg_color(view.root, active ? lv_color_mix(spec.colour, UI_COL_SURFACE, 48) : UI_COL_SURFACE,
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(view.root, active ? spec.colour : UI_COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_text_color(view.icon, active ? spec.colour : (usable ? UI_COL_TEXT_DIM : UI_COL_TEXT_FAINT),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(view.title, usable ? UI_COL_TEXT : UI_COL_TEXT_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_color(view.corner, active ? spec.colour : UI_COL_TEXT_DIM, LV_PART_MAIN);
    }
}

lv_obj_t *ScreenMenu_create(lv_obj_t *parent)
{
    Pager_create(pager, parent, PageCount);
    for (int i = 0; i < TileCount; ++i)
    {
        buildTile(i);
    }
    ScreenMenu_refresh();
    return pager.root;
}

void ScreenMenu_refresh()
{
    for (int i = 0; i < TileCount; ++i)
    {
        // Only the visible page: the other is not on screen and costs nothing
        // to leave stale until it is.
        if (i / TilesPerPage == pager.current)
        {
            refreshTile(i);
        }
    }
}

void ScreenMenu_home()
{
    Pager_go(pager, 0);
}
