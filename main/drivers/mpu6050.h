#pragma once

#include "esp_err.h"
#include <stdint.h>

/*============================================================================
 * I2C Address
 *============================================================================*/
#define MPU6050_I2C_ADDR     0x68   /* AD0 low  (default) */
#define MPU6050_I2C_ADDR_ALT 0x69   /* AD0 high */
#define MPU6050_I2C_FREQ_HZ  400000

/*============================================================================
 * Register Map (subset)
 *============================================================================*/
#define MPU6050_REG_WHO_AM_I   0x75  /* Should read 0x68 */
#define MPU6050_WHO_AM_I_VALUE 0x68

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Probe the I2C bus for an MPU6050 at 0x68/0x69 and confirm via WHO_AM_I.
 *        Does NOT require the sensor to be awake (WHO_AM_I is readable in sleep).
 * @return ESP_OK if a device ACKs and WHO_AM_I matches,
 *         ESP_ERR_NOT_FOUND if no device responds.
 */
esp_err_t mpu6050_probe(void);
