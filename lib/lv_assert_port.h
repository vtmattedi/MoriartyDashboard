#pragma once

// What LVGL does when one of its asserts fails -- in practice, almost always an
// allocation that came back NULL. Wired in by LV_ASSERT_HANDLER in lv_conf.h and
// implemented in lib/boardstuff/boardstuff.cpp.
//
// LVGL's default handler is `while(1);`, which on this panel froze the loop()
// task for good: touch and Serial stopped while the MQTT tasks kept running and
// logging, so the panel looked alive but ignored everything. This logs the heap
// state and restarts instead.
//
// Declared C-compatible because it is called from LVGL, which is C.

#ifdef __cplusplus
extern "C"
{
#endif

void lvgl_assert_failed(void);

#ifdef __cplusplus
}
#endif
