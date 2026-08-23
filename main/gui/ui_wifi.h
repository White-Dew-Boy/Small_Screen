#pragma once
#include "lvgl.h"

/**
 * @brief Create the WiFi status page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_wifi_create(void);

/**
 * @brief Register a callback invoked when the "Saved WiFi" button is
 *        pressed. The callback must switch to the saved-networks page
 *        from the LVGL thread.
 */
void ui_wifi_set_saved_cb(void (*cb)(void));

/**
 * @brief Register a callback invoked when the "Nearby WiFi" button is
 *        pressed. The callback must switch to the nearby-scan page from
 *        the LVGL thread.
 */
void ui_wifi_set_nearby_cb(void (*cb)(void));
