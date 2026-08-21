#pragma once

/**
 * @brief Create the SHTC3 sensor dashboard UI on the current LVGL screen.
 *        Must be called after lv_init() and lv_port_disp_init().
 */
void ui_shtc3_create(void);

/**
 * @brief Publish fresh sensor data from the sensor task.
 *        Safe to call from any task: data is shared via volatile globals
 *        and only consumed inside the LVGL timer callback.
 *
 * @param temp_c  Temperature in degrees Celsius.
 * @param humi_rh Relative humidity in percent.
 */
void ui_shtc3_set_data(float temp_c, float humi_rh);

/**
 * @brief Mark the sensor data as invalid (e.g. after a read failure).
 */
void ui_shtc3_set_invalid(void);
