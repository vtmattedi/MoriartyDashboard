#pragma once
#include <lvgl.h>

// Shared look for every screen.
//
// Dark by default: this panel is looked at in a dim room more often than not,
// and a white screen at night is a lamp.

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

// Portrait layout: 320x480, held in the hand. Top bar along the top, pages below.
//
// Width is the scarce dimension now (304px of card) and height the plentiful
// one (396px of content), which turns the old two-column pages into one column
// of stacked cards, and lets the nav bar be deep enough to put each tab's icon
// above its name -- four of them side by side do not fit across 320px.
//
// Anything taller than the page scrolls. Nothing is sized as a percentage of a
// height that depends on its own children; see the notes where bodies are built.
// There is no nav bar any more: the top bar has a back button, and the menu is the
// way to everything else.
#define UI_NAVBAR_H 0
#define UI_STATUSBAR_H 28
#define UI_GUTTER 8

#define UI_CONTENT_W SCREEN_WIDTH
#define UI_CONTENT_H (SCREEN_HEIGHT - UI_STATUSBAR_H - UI_NAVBAR_H)

/// Full-width card inside the content area.
#define UI_CARD_W (UI_CONTENT_W - (2 * UI_GUTTER))
/// Width left inside a card once its padding and border are taken off.
#define UI_CARD_INNER (UI_CARD_W - 22)
/// Height of a page inside a Pager (the dots row is taken off).
#define UI_PAGER_DOTS_H 26
#define UI_PAGE_H_PAGER (UI_CONTENT_H - UI_PAGER_DOTS_H)
/// Height left for a page's cards once its own padding is taken off.
#define UI_PAGE_H (UI_CONTENT_H - (2 * UI_GUTTER))

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

/// @brief Let a container scroll vertically when its children outgrow it, and
/// only vertically. `scrollbar` is for cards, where it is the only sign that
/// there is more below; whole pages leave it off.
void Theme_scrollVertical(lv_obj_t *obj, bool scrollbar);

/// @brief Set a label's text only if it differs. lv_label_set_text reallocates
/// and redraws even for identical text, which on a refresh loop running several
/// times a second is steady heap churn for nothing.
void Theme_setText(lv_obj_t *label, const char *text);
