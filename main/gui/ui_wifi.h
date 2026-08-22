#pragma once
#include "lvgl.h"

/**
 * @brief Create the WiFi status page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_wifi_create(void);
