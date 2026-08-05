#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"

esp_err_t lcd_driver_init(esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io);
