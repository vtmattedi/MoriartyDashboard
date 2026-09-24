#pragma once
#include <lvgl.h>

// Shared look for every screen.
//
// Dark by default: this panel lives on a wall and is looked at in a dim room
// more often than not, and a white screen at night is a lamp.

#define UI_COL_BG lv_color_hex(0x0E1116)
#define UI_COL_SURFACE lv_color_hex(0x171C24)
#define UI_COL_SURFACE_ALT lv_color_hex(0x212936)
#define UI_COL_BORDER lv_color_hex(0x2A3441)
#define UI_COL_TEXT lv_color_hex(0xE8EDF5)
#define UI_COL_TEXT_DIM lv_color_hex(0x8A97A8)
#define UI_COL_TEXT_FAINT lv_color_hex(0x5A6678)
// The NightMare mark's orange, so the panel matches the web dashboard.
#define UI_COL_ACCENT lv_color_hex(0xFF9849)
#define UI_COL_OK lv_color_hex(0x37C978)
#define UI_COL_WARN lv_color_hex(0xFFC64B)
#define UI_COL_DANGER lv_color_hex(0xF2566A)
#define UI_COL_COOL lv_color_hex(0x4FB0F5)

// Landscape layout: 480x320. Status bar along the top, nav bar along the bottom,
// pages in between.
//
// That leaves height as the scarce dimension (246px of content) and width to
// spare (464px), which is why most pages are two columns and why the nav bar is
// kept shallow.
#define UI_NAVBAR_H 46
#define UI_STATUSBAR_H 28
#define UI_GUTTER 8

#define UI_CONTENT_W SCREEN_WIDTH
#define UI_CONTENT_H (SCREEN_HEIGHT - UI_STATUSBAR_H - UI_NAVBAR_H)

/// Full-width card inside the content area.
#define UI_CARD_W (UI_CONTENT_W - (2 * UI_GUTTER))
/// One of two side-by-side columns.
#define UI_COL_W ((UI_CARD_W - UI_GUTTER) / 2)

/// @brief Build the shared styles. Call once, before creating any screen.
void Theme_init();

/// @brief A rounded surface panel with the standard border and padding.
/// Pass LV_SIZE_CONTENT for either dimension to size it to its children.
lv_obj_t *Theme_card(lv_obj_t *parent, lv_coord_t width, lv_coord_t height);

/// @brief A transparent flex container, for grouping without a card's chrome.
lv_obj_t *Theme_column(lv_obj_t *parent, lv_coord_t width, lv_coord_t height);
lv_obj_t *Theme_row(lv_obj_t *parent, lv_coord_t width, lv_coord_t height);

/// @brief A card's heading row: returns the row, with `title` already placed in it.
lv_obj_t *Theme_cardHeader(lv_obj_t *card, const char *title);

/// @brief A small rounded status pill. Set its text and colour with Theme_chipSet().
lv_obj_t *Theme_chip(lv_obj_t *parent);
void Theme_chipSet(lv_obj_t *chip, const char *text, lv_color_t colour);

/// @brief A flat text button sized for a finger.
lv_obj_t *Theme_button(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h);

/// @brief A label with one of the standard roles applied.
lv_obj_t *Theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t colour);

/// @brief Strip every default scroll/click behaviour from a layout-only container.
void Theme_plainContainer(lv_obj_t *obj);

/// @brief Set a label's text only if it differs. lv_label_set_text reallocates
/// and redraws even for identical text, which on a refresh loop running several
/// times a second is steady heap churn for nothing.
void Theme_setText(lv_obj_t *label, const char *text);
