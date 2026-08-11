#include "ili9341_lvgl.h"

#include "lcd_driver.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "ili9341_lvgl";

#define LCD_H_RES  240
#define LCD_V_RES  320

/* LVGL draw buffers — 1/10 screen lines each, partial mode */
#define LVGL_BUF_LINES  32
static lv_color_t buf1[LCD_H_RES * LVGL_BUF_LINES];
static lv_color_t buf2[LCD_H_RES * LVGL_BUF_LINES];

/*============================================================================
 * Flush callback — LVGL calls this to push pixels to the display
 *============================================================================*/
static void ili9341_flush_cb(lv_display_t *disp, const lv_area_t *area,
                              uint8_t *px_map)
{
    int w = lv_area_get_width(area);
    int h = lv_area_get_height(area);

    /* Byte-swap RGB565 for ILI9341 (LVGL native is little-endian,
     * ILI9341 expects big-endian over SPI) */
    uint16_t *pixels = (uint16_t *)px_map;
    for (int i = 0; i < w * h; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                               area->x2 + 1, area->y2 + 1, pixels);
    lv_display_flush_ready(disp);
}

/*============================================================================
 * Public API
 *============================================================================*/
esp_err_t ili9341_lvgl_init(void)
{
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, ili9341_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1),
                            LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "LVGL display registered (ILI9341, %dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
