#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include "drivers/jy901s.h"

/**
 * @brief Create the sensor dashboard UI (SHTC3 + JY901S attitude).
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sensor_create(void);

/**
 * @brief Publish fresh SHTC3 data from the sensor task.
 *        Safe to call from any task: data is shared via volatile globals
 *        and only consumed inside the LVGL timer callback.
 *
 * @param temp_c  Temperature in degrees Celsius.
 * @param humi_rh Relative humidity in percent.
 */
void ui_sensor_set_data(float temp_c, float humi_rh);

/**
 * @brief Publish fresh JY901S data from the IMU task (same sharing scheme).
 *
 * @param imu    Latest IMU sample (attitude, accel, gyro, temperature).
 * @param online Whether the IMU is currently receiving valid frames.
 */
void ui_sensor_set_imu_data(const jy901s_data_t *imu, bool online);

/**
 * @brief Register a callback invoked when the "Accel" button is pressed.
 *        The callback must switch to the acceleration page from the LVGL thread.
 */
void ui_sensor_set_accel_cb(void (*cb)(void));

/**
 * @brief Register a callback invoked when the "Gyro" button is pressed.
 *        The callback must switch to the angular velocity page from the LVGL thread.
 */
void ui_sensor_set_gyro_cb(void (*cb)(void));

/**
 * @brief Mark the SHTC3 sensor data as invalid (e.g. after a read failure).
 */
void ui_sensor_set_invalid(void);
