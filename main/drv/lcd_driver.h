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

/**
 * @brief Clear any stale flush-completion signal. Call before starting a
 *        new esp_lcd_panel_draw_bitmap() transfer.
 */
void lcd_flush_begin(void);

/**
 * @brief Block until the current color transfer has fully completed.
 *        The LCD and SD share one SPI bus; waiting here (and running all
 *        SD probing from the same LVGL task) prevents concurrent SPI use,
 *        which would otherwise trip an ESP-IDF SPI assert.
 */
void lcd_flush_wait(void);
