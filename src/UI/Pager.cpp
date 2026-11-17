#include "Pager.h"
#include "Sys.h"
#include <boardstuff.h>
#include "Theme.h"

namespace
{
    uint32_t lastSwipeMs = 0;
    bool swiped = false;

    void onGesture(lv_event_t *event)
    {
        Pager *p = (Pager *)lv_event_get_user_data(event);
        const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_get_act());
        int target = p->current;
        if (direction == LV_DIR_LEFT)
        {
            target = p->current + 1;
        }
        else if (direction == LV_DIR_RIGHT)
        {
            target = p->current - 1;
        }
        else
        {
            return;
        }
        swiped = true;
        lastSwipeMs = millisNow();
        Pager_go(*p, target);
    }
}

namespace
{
    void onSwipeGesture(lv_event_t *event)
    {
        const SwipeHandlers *handlers = (const SwipeHandlers *)lv_event_get_user_data(event);
        const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (direction == LV_DIR_LEFT && handlers->next)
        {
            swiped = true;
            lastSwipeMs = millisNow();
            handlers->next();
        }
        else if (direction == LV_DIR_RIGHT && handlers->previous)
        {
            swiped = true;
            lastSwipeMs = millisNow();
            handlers->previous();
        }
    }
}

void Pager_onSwipe(lv_obj_t *obj, const SwipeHandlers *handlers)
{
    lv_obj_add_event_cb(obj, onSwipeGesture, LV_EVENT_GESTURE, (void *)handlers);
    // See Pager_create: the gesture goes to the first parent that does not pass it up.
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

lv_obj_t *Pager_create(Pager &p, lv_obj_t *parent, int count)
{
    if (count > PAGER_MAX)
    {
        count = PAGER_MAX;
    }
    p.count = count;
    p.current = 0;

    p.root = lv_obj_create(parent);
    Theme_plainContainer(p.root);
    lv_obj_set_size(p.root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_event_cb(p.root, onGesture, LV_EVENT_GESTURE, &p);
    // LVGL sends a gesture to the first parent that does NOT pass it up, so the
    // pager has to be that parent. Every object is created passing gestures to
    // its parent, which sent them past this one to the bare screen.
    lv_obj_clear_flag(p.root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    const lv_coord_t pageHeight = UI_CONTENT_H - UI_PAGER_H;
    for (int i = 0; i < count; ++i)
    {
        lv_obj_t *page = lv_obj_create(p.root);
        Theme_plainContainer(page);
        lv_obj_set_size(page, UI_CONTENT_W, pageHeight);
        lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, 0);
        if (i != 0)
        {
            lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
        }
        p.page[i] = page;
    }

    if (count > 1)
    {
        lv_obj_t *dots = lv_obj_create(p.root);
        Theme_plainContainer(dots);
        lv_obj_clear_flag(dots, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dots, UI_CONTENT_W, UI_PAGER_H);
        lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dots, 10, LV_PART_MAIN);
        for (int i = 0; i < count; ++i)
        {
            lv_obj_t *dot = lv_obj_create(dots);
            lv_obj_remove_style_all(dot);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
            p.dot[i] = dot;
        }
    }
    Pager_go(p, 0);
    return p.root;
}

void Pager_go(Pager &p, int index)
{
    if (index < 0)
    {
        index = 0;
    }
    if (index >= p.count)
    {
        index = p.count - 1;
    }
    for (int i = 0; i < p.count; ++i)
    {
        if (i == index)
        {
            lv_obj_clear_flag(p.page[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(p.page[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (p.dot[i])
        {
            lv_obj_set_style_bg_color(p.dot[i], i == index ? UI_COL_ACCENT : UI_COL_TEXT_FAINT, LV_PART_MAIN);
        }
    }
    p.current = index;
}

void Pager_noSwipe(lv_obj_t *widget)
{
    lv_obj_clear_flag(widget, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

bool Pager_justSwiped()
{
    return swiped && (millisNow() - lastSwipeMs) < 350;
}
