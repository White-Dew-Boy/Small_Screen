#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define LCD_H_RES  240
#define LCD_V_RES  320

extern esp_lcd_panel_handle_t panel_handle;

esp_err_t lcd_init(void);
void gb_swap_buf(uint16_t *pixels, int count);
