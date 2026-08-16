#include "lcd_driver.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include <string.h>
#include <stdlib.h>

#define LCD_HOST   SPI3_HOST
#define LCD_MOSI   38
#define LCD_MISO   41
#define LCD_SCLK   39
#define LCD_CS     21
#define LCD_DC     48
#define LCD_RST    47
#define LCD_BL     40

static const char *TAG = "lcd_driver";
esp_lcd_panel_handle_t panel_handle;

#define LCD_H_RES  240
#define LCD_V_RES  320

void gb_swap_buf(uint16_t *pixels, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t c = pixels[i];
        uint16_t r_out = (c & 0x001F) << 11;
        uint16_t g_out = (c & 0x07E0);
        uint16_t b_out = (c & 0xF800) >> 11;
        pixels[i] = __builtin_bswap16(r_out | g_out | b_out);
    }
}

esp_err_t lcd_driver_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Verify SPI bus is already initialized by sd_card_init() */
    size_t dummy;
    if (spi_bus_get_max_transaction_len(LCD_HOST, &dummy) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus not initialized — call sd_card_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x21, NULL, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BL,
        .duty = 1023,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "LCD driver initialized");
    return ESP_OK;
}

static uint16_t *fb;

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w > LCD_H_RES ? LCD_H_RES : x + w;
    int y2 = y + h > LCD_V_RES ? LCD_V_RES : y + h;
    for (int row = y1; row < y2; row++)
        for (int col = x1; col < x2; col++)
            fb[row * LCD_H_RES + col] = color;
}

static void flush_rect(int x, int y, int w, int h)
{
    int count = w * h;
    uint16_t *tmp = malloc(count * sizeof(uint16_t));
    if (!tmp) return;
    for (int row = 0; row < h; row++)
        memcpy(&tmp[row * w], &fb[(y + row) * LCD_H_RES + x], w * sizeof(uint16_t));
    gb_swap_buf(tmp, count);
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, tmp);
    free(tmp);
}

esp_err_t lcd_init(void)
{
    esp_err_t ret = lcd_driver_init();
    if (ret != ESP_OK) return ret;

    fb = calloc(1, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    assert(fb);
    flush_rect(0, 0, LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

void lcd_fill_screen(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) fb[i] = color;
    flush_rect(0, 0, LCD_H_RES, LCD_V_RES);
}

static void erase_region(int16_t cx, int16_t cy)
{
    fill_rect(cx - 20, cy - 20, 40, 40, 0x0000);
}

static void draw_cross(int16_t cx, int16_t cy, uint16_t color)
{
    fill_rect(cx - 20, cy - 2, 40, 4, color);
    fill_rect(cx - 2, cy - 20, 4, 40, color);
}

void lcd_draw_cross(int16_t cx, int16_t cy, uint16_t color)
{
    if (cx < 20) cx = 20;
    if (cx > LCD_H_RES - 21) cx = LCD_H_RES - 21;
    if (cy < 20) cy = 20;
    if (cy > LCD_V_RES - 21) cy = LCD_V_RES - 21;

    draw_cross(cx, cy, color);
    flush_rect(cx - 20, cy - 20, 40, 40);
}

void lcd_move_cross(int16_t old_cx, int16_t old_cy,
                    int16_t new_cx, int16_t new_cy)
{
    if (new_cx < 20) new_cx = 20;
    if (new_cx > LCD_H_RES - 21) new_cx = LCD_H_RES - 21;
    if (new_cy < 20) new_cy = 20;
    if (new_cy > LCD_V_RES - 21) new_cy = LCD_V_RES - 21;

    int nx0 = new_cx - 20, ny0 = new_cy - 20;

    if (old_cx >= 0) {
        if (old_cx < 20) old_cx = 20;
        if (old_cx > LCD_H_RES - 21) old_cx = LCD_H_RES - 21;
        if (old_cy < 20) old_cy = 20;
        if (old_cy > LCD_V_RES - 21) old_cy = LCD_V_RES - 21;

        int ox0 = old_cx - 20, oy0 = old_cy - 20;
        erase_region(old_cx, old_cy);

        int x1 = ox0 < nx0 ? ox0 : nx0;
        int y1 = oy0 < ny0 ? oy0 : ny0;
        int x2 = ox0 + 40 > nx0 + 40 ? ox0 + 40 : nx0 + 40;
        int y2 = oy0 + 40 > ny0 + 40 ? oy0 + 40 : ny0 + 40;

        draw_cross(new_cx, new_cy, 0xF800);
        flush_rect(x1, y1, x2 - x1, y2 - y1);
    } else {
        draw_cross(new_cx, new_cy, 0xF800);
        flush_rect(nx0, ny0, 40, 40);
    }
}

void lcd_clear_cross(int16_t cx, int16_t cy)
{
    if (cx < 20) cx = 20;
    if (cx > LCD_H_RES - 21) cx = LCD_H_RES - 21;
    if (cy < 20) cy = 20;
    if (cy > LCD_V_RES - 21) cy = LCD_V_RES - 21;

    erase_region(cx, cy);
    flush_rect(cx - 20, cy - 20, 40, 40);
}
