#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t lcd_driver_init(void);
void lcd_draw_bitmap(uint16_t *pixels, int x, int y, int w, int h);
void lcd_lvgl_init(void);
void lcd_lvgl_touch_init(void);
void lcd_lvgl_touch_poll(void);
