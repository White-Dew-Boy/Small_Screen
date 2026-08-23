#pragma once
#include "lvgl.h"

/**
 * @brief Create the MQTT command history page: publish counter plus the
 *        most recent received commands.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_mqtt_history_create(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the MQTT status page from the
 *        LVGL thread.
 */
void ui_mqtt_history_set_back_cb(void (*cb)(void));
