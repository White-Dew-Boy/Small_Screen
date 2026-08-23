#pragma once
#include "lvgl.h"

/**
 * @brief Create the WiFi configuration page (SSID/password entry with
 *        an on-screen keyboard).
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_wifi_config_create(void);

/**
 * @brief Register a callback invoked when the user wants to leave the
 *        config page (Back button, or successful connect). The callback
 *        must switch back to the WiFi status page from the LVGL thread.
 */
void ui_wifi_config_set_back_cb(void (*cb)(void));
