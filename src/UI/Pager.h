#pragma once
#include <stdint.h>
#include <lvgl.h>

// Pages that flip instead of scroll.
//
// A scroll redraws the whole 320x480 screen every frame, and the SPI link alone
// needs about 60 ms to send that, so it can never feel smooth on this panel. A
// page flip draws once. A Pager is a stack of full-size pages with a row of dots
// at the bottom; a horizontal swipe moves between them.

#define PAGER_MAX 8
/// Height of the dots row at the bottom of a pager.
#define UI_PAGER_H 26

struct Pager
{
    lv_obj_t *root = nullptr;
    lv_obj_t *page[PAGER_MAX] = {nullptr};
    lv_obj_t *dot[PAGER_MAX] = {nullptr};
    int count = 0;
    int current = 0;
};

/// @brief Fill `parent` with `count` pages. Returns the root; the pages are in p.page[].
/// Each page is a transparent plain container, the full width and UI_PAGER_H shorter than the parent.
lv_obj_t *Pager_create(Pager &p, lv_obj_t *parent, int count);

/// @brief Show page `index` (clamped).
void Pager_go(Pager &p, int index);

/// What a screen that pages its own rows does on a swipe.
struct SwipeHandlers
{
    void (*next)();
    void (*previous)();
};

/// @brief Call `handlers` when `obj` is swiped sideways (left = next). `obj`
/// must be the topmost thing that should catch it, and the handlers must outlive it.
void Pager_onSwipe(lv_obj_t *obj, const SwipeHandlers *handlers);

/// @brief Stop a widget that is dragged sideways (a slider, a colour wheel) from
/// also being read as a page swipe.
void Pager_noSwipe(lv_obj_t *widget);

/// @brief True for a moment after a swipe. A tap handler checks it so that the
/// finger lifting at the end of a swipe is not also a press.
bool Pager_justSwiped();
