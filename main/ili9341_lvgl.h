#pragma once

#include "esp_err.h"

/**
 * @brief Register the ILI9341 as an LVGL display, using the existing
 *        esp_lcd panel handle from lcd_driver.
 * @return ESP_OK on success.
 */
esp_err_t ili9341_lvgl_init(void);
