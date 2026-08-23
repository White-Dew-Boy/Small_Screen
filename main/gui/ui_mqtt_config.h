#pragma once
#include "lvgl.h"

/**
 * @brief Create the MQTT connection config page: edit protocol, address,
 *        port, path, username, password, client id and keepalive with the
 *        on-screen keyboard. Save applies the new config immediately.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_mqtt_config_create(void);

/**
 * @brief Register a callback invoked when leaving the page (Save or Back).
 *        The callback must switch back to the MQTT status page from the
 *        LVGL thread.
 */
void ui_mqtt_config_set_back_cb(void (*cb)(void));
