#pragma once
#include "lvgl.h"

/**
 * @brief Create the MQTT status page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_mqtt_create(void);

/**
 * @brief Register a callback invoked when the "Interval" button is pressed.
 *        The callback must switch to the interval settings page from the
 *        LVGL thread.
 */
void ui_mqtt_set_interval_cb(void (*cb)(void));

/**
 * @brief Register a callback invoked when the "History" button is pressed.
 *        The callback must switch to the command history page from the
 *        LVGL thread.
 */
void ui_mqtt_set_history_cb(void (*cb)(void));

/**
 * @brief Register a callback invoked when the "Config" button is pressed.
 *        The callback must switch to the connection config page from the
 *        LVGL thread.
 */
void ui_mqtt_set_config_cb(void (*cb)(void));
