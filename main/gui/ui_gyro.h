#pragma once
#include "lvgl.h"

/**
 * @brief Create the 3-axis angular velocity page (JY901S), entered from the
 *        Sensor page's "Gyro" button.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_gyro_create(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the Sensor page from the LVGL thread.
 */
void ui_gyro_set_back_cb(void (*cb)(void));
