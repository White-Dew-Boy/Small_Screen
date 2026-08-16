#pragma once

#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration (from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define GPA_IMU_GPIO     GPIO_NUM_7    /* GPA0: attitude sensor power */
#define GPA_TEMP_GPIO    GPIO_NUM_15   /* GPA1: temperature sensor power */
#define GPA_AUDIO_GPIO   GPIO_NUM_10   /* GPA2: MAX98357A audio amp power */
#define GPA_LCD_GPIO     GPIO_NUM_11   /* GPA3: LCD module power */

/*============================================================================
 * Data Structures
 *============================================================================*/
typedef enum {
    GPA_ID_IMU = 0,
    GPA_ID_TEMP,
    GPA_ID_AUDIO,
    GPA_ID_LCD,
    GPA_ID_NUM,
} gpa_id_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Configure all GPA power pins as push-pull outputs and turn them off.
 * @return ESP_OK on success.
 */
esp_err_t gpa_init(void);

/**
 * @brief Power on a module (drive its GPA pin high).
 * @param[in] id  Module id (GPA_ID_IMU .. GPA_ID_LCD).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if id out of range.
 */
esp_err_t gpa_power_on(gpa_id_t id);

/**
 * @brief Power off a module (drive its GPA pin low).
 * @param[in] id  Module id (GPA_ID_IMU .. GPA_ID_LCD).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if id out of range.
 */
esp_err_t gpa_power_off(gpa_id_t id);
