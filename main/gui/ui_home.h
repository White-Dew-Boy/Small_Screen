#pragma once
#include "lvgl.h"

/* Page indexes of the KEY2 cycle (must match main.c) */
#define HOME_PAGE_SENSOR 0
#define HOME_PAGE_WIFI   1
#define HOME_PAGE_MQTT   2
#define HOME_PAGE_LED    3

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
