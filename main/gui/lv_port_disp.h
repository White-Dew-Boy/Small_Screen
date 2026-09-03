#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t lv_port_disp_init(void);
esp_err_t lv_port_tick_init(void);

/**
 * @brief Switch the whole display between portrait (240x320, default) and
 *        landscape (320x240) at runtime. Used by the Home menu and the
 *        PC-Perf page so they can render wide while every other page stays
 *        portrait.
 *
 *        Combination of LVGL's lv_disp_set_rotation() (coherent state:
 *        resolution swap, all screens resized, invalidation reset) with
 *        hardware rotation of the ILI9341 panel via MADCTL (swap_xy +
 *        mirror). LVGL's own software pixel rotation is NOT used (sw_rotate
 *        stays 0 - it garbles with this project's partial draw buffer);
 *        the flush receives unrotated logical pixels which the rotated
 *        panel displays correctly.
 *        Call with true BEFORE loading the landscape screen, and with false
 *        BEFORE loading any portrait screen afterwards.
 *
 * @param landscape true = 320x240 landscape, false = 240x320 portrait
 * @return ESP_OK on success
 */
esp_err_t lv_port_set_landscape(bool landscape);
