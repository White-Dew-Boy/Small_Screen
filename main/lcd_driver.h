#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

extern esp_lcd_panel_handle_t panel_handle;

esp_err_t lcd_driver_init(void);
esp_err_t lcd_init(void);
void lcd_fill_screen(uint16_t color);
void lcd_draw_cross(int16_t cx, int16_t cy, uint16_t color);
void lcd_move_cross(int16_t old_cx, int16_t old_cy,
                    int16_t new_cx, int16_t new_cy);
void lcd_clear_cross(int16_t cx, int16_t cy);