#pragma once

#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration (from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define POWER_KEY1_GPIO   GPIO_NUM_4    /* MP2636 MODE power-on latch */
#define POWER_IMU_GPIO    GPIO_NUM_15    /* GPA0: attitude sensor power */
#define POWER_TEMP_GPIO   GPIO_NUM_7   /* GPA1: temperature sensor power */
#define POWER_AUDIO_GPIO  GPIO_NUM_11   /* GPA2: MAX98357A audio amp power */
#define POWER_LCD_GPIO    GPIO_NUM_10   /* GPA3: LCD module power */

/*============================================================================
 * Data Structures
 *============================================================================*/
typedef enum {
    POWER_ID_IMU = 0,
    POWER_ID_TEMP,
    POWER_ID_AUDIO,
    POWER_ID_LCD,
    POWER_ID_NUM,
} power_id_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Latch KEY1 high (power-on) and configure all GPA pins as outputs, off.
 * @return ESP_OK on success.
 */
esp_err_t power_init(void);

/**
 * @brief Power on a module (drive its GPA pin high).
 * @param[in] id  Module id (POWER_ID_IMU .. POWER_ID_LCD).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if id out of range.
 */
esp_err_t power_on(power_id_t id);

/**
 * @brief Power off a module (drive its GPA pin low).
 * @param[in] id  Module id (POWER_ID_IMU .. POWER_ID_LCD).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if id out of range.
 */
esp_err_t power_off(power_id_t id);

/**
 * @brief Enter deep sleep until one of the buttons is pressed.
 *
 * Stops WiFi, powers off all modules, and configures EXT1 GPIO wake-up on
 * KEY2/KEY3 (active-low, i.e. pressing either button wakes the chip).
 * After wake the chip restarts from app_main, which re-initializes all
 * modules.
 *
 * @warning Does not return.
 */
void power_deep_sleep(void);
