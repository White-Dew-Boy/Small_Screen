#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t lcd_driver_init(void);
void lcd_draw_bitmap(uint16_t *pixels, int x, int y, int w, int h);
void lcd_lvgl_init(void);
void lcd_lvgl_touch_init(void);
void lcd_lvgl_touch_poll(void);
bool lcd_touch_is_pressed(void);
void lcd_touch_get_pos(int16_t *x, int16_t *y);
