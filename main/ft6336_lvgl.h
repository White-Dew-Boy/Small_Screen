#pragma once

#include "esp_err.h"

/**
 * @brief Register the FT6336 touch as an LVGL input device.
 * @return ESP_OK on success.
 */
esp_err_t ft6336_lvgl_init(void);
