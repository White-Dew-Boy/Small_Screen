#pragma once
#include "lvgl.h"

/* Page indexes of the KEY2 cycle (must match main.c) */
#define HOME_PAGE_SENSOR  0
#define HOME_PAGE_WIFI    1
#define HOME_PAGE_MQTT    2
#define HOME_PAGE_LED     3
#define HOME_PAGE_SD      4
#define HOME_PAGE_SYSINFO 5

/**
 * @brief Create the home menu page listing all main screens. Tapping an
 *        entry switches to that page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_home_create(void);

/**
 * @brief Register a callback invoked when a menu entry is tapped.
 *        The callback receives the page index (HOME_PAGE_*) and must
 *        switch to that page from the LVGL thread.
 */
void ui_home_set_page_cb(void (*cb)(int page));

/**
 * @brief Register a callback invoked when the "Deep Sleep" button is
 *        pressed. The callback must configure the wake-up source and put
 *        the chip into deep sleep.
 */
void ui_home_set_deepsleep_cb(void (*cb)(void));

/**
 * @brief Reset the deep-sleep press counter and restore the default
 *        bottom hint. Call when the page is re-shown (e.g. KEY3 back to
 *        Home) so stale "Press N more to sleep" text clears.
 */
void ui_home_reset_hint(void);
