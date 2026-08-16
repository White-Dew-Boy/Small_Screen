#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Parameters (from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define RGB_LED_GPIO   GPIO_NUM_8
#define RGB_LED_NUM    3

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize the WS2812 strip (RGB_LED_NUM LEDs) on the RMT backend.
 * @return ESP_OK on success.
 */
esp_err_t rgb_led_init(void);

/**
 * @brief Stage the color of one LED. Call rgb_led_refresh() to apply.
 * @param[in] index  LED index (0 .. RGB_LED_NUM-1).
 * @param[in] r,g,b  8-bit color components.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if index out of range.
 */
esp_err_t rgb_led_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Stage the same color for all LEDs. Call rgb_led_refresh() to apply.
 * @param[in] r,g,b  8-bit color components.
 * @return ESP_OK on success.
 */
esp_err_t rgb_led_set_all(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Push the staged colors to the strip.
 * @return ESP_OK on success.
 */
esp_err_t rgb_led_refresh(void);

/**
 * @brief Turn off all LEDs and apply immediately.
 * @return ESP_OK on success.
 */
esp_err_t rgb_led_clear(void);
