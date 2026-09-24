#include "Theme.h"
#include <boardstuff.h>
#include <string.h>

namespace
{
    lv_style_t styleCard;
    lv_style_t styleChip;
    lv_style_t styleButton;
    lv_style_t styleButtonPressed;
    lv_style_t styleButtonDisabled;
    bool ready = false;
}

void Theme_init()
{
    if (ready)
    {
        return;
    }
    ready = true;

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, UI_COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, UI_COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_14, LV_PART_MAIN);

    lv_style_init(&styleCard);
    lv_style_set_bg_color(&styleCard, UI_COL_SURFACE);
    lv_style_set_bg_opa(&styleCard, LV_OPA_COVER);
    lv_style_set_border_color(&styleCard, UI_COL_BORDER);
    lv_style_set_border_width(&styleCard, 1);
    lv_style_set_radius(&styleCard, 12);
    lv_style_set_pad_all(&styleCard, 10);
    lv_style_set_pad_row(&styleCard, 8);

    lv_style_init(&styleChip);
    lv_style_set_bg_opa(&styleChip, LV_OPA_20);
    lv_style_set_radius(&styleChip, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&styleChip, 10);
    lv_style_set_pad_ver(&styleChip, 4);
    lv_style_set_border_width(&styleChip, 0);

    lv_style_init(&styleButton);
    lv_style_set_bg_color(&styleButton, UI_COL_SURFACE_ALT);
    lv_style_set_bg_opa(&styleButton, LV_OPA_COVER);
    lv_style_set_border_color(&styleButton, UI_COL_BORDER);
    lv_style_set_border_width(&styleButton, 1);
    lv_style_set_radius(&styleButton, 10);
    lv_style_set_text_color(&styleButton, UI_COL_TEXT);
    lv_style_set_shadow_width(&styleButton, 0);

    // Touch feedback has to be obvious: there is no cursor and no hover on a
    // panel, so the press state is the only confirmation a tap registered.
    lv_style_init(&styleButtonPressed);
    lv_style_set_bg_color(&styleButtonPressed, UI_COL_ACCENT);
    lv_style_set_text_color(&styleButtonPressed, UI_COL_BG);
    lv_style_set_border_color(&styleButtonPressed, UI_COL_ACCENT);

    lv_style_init(&styleButtonDisabled);
    lv_style_set_bg_color(&styleButtonDisabled, UI_COL_SURFACE);
    lv_style_set_text_color(&styleButtonDisabled, UI_COL_TEXT_FAINT);
    lv_style_set_border_color(&styleButtonDisabled, UI_COL_SURFACE_ALT);
    lv_style_set_opa(&styleButtonDisabled, LV_OPA_60);
}

void Theme_plainContainer(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
}

lv_obj_t *Theme_card(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &styleCard, LV_PART_MAIN);
    lv_obj_set_size(card, width, height);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return card;
}

/// Layout-only containers must not take clicks. lv_obj_create makes every
/// object clickable by default, so without this a tap on the text inside a
/// clickable card lands on the row or column holding it and never reaches the
/// card -- the card only responds on its padding. Not applied in
/// Theme_plainContainer itself: scrollable pages are built from that too, and a
/// page that cannot be pressed cannot be scrolled from its empty space.
static void passClicksThrough(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *Theme_column(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *column = lv_obj_create(parent);
    Theme_plainContainer(column);
    passClicksThrough(column);
    lv_obj_set_size(column, width, height);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, UI_GUTTER, LV_PART_MAIN);
    return column;
}

lv_obj_t *Theme_row(lv_obj_t *parent, lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    Theme_plainContainer(row);
    passClicksThrough(row);
    lv_obj_set_size(row, width, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

void Theme_setText(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0)
    {
        lv_label_set_text(label, text);
    }
}

lv_obj_t *Theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour, LV_PART_MAIN);
    return label;
}

lv_obj_t *Theme_cardHeader(lv_obj_t *card, const char *title)
{
    lv_obj_t *row = lv_obj_create(card);
    Theme_plainContainer(row);
    passClicksThrough(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    Theme_label(row, title, &lv_font_montserrat_16, UI_COL_TEXT);
    return row;
}

lv_obj_t *Theme_chip(lv_obj_t *parent)
{
    lv_obj_t *chip = lv_label_create(parent);
    lv_obj_add_style(chip, &styleChip, LV_PART_MAIN);
    lv_obj_set_style_text_font(chip, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(chip, "");
    return chip;
}

void Theme_chipSet(lv_obj_t *chip, const char *text, lv_color_t colour)
{
    lv_label_set_text(chip, text);
    lv_obj_set_style_text_color(chip, colour, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, colour, LV_PART_MAIN);
}

lv_obj_t *Theme_button(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &styleButton, LV_PART_MAIN);
    lv_obj_add_style(button, &styleButtonPressed, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(button, &styleButtonDisabled, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_size(button, w, h);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}
