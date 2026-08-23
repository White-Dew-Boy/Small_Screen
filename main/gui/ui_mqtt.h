#pragma once
#include "lvgl.h"

/**
 * @brief Create the MQTT status page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_mqtt_create(void);
