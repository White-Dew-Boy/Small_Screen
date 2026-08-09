#include "lcd_driver.h"
#include "ft6336.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "lvgl.h"

#define LCD_HOST   SPI3_HOST
#define LCD_MOSI   38
#define LCD_MISO   41
#define LCD_SCLK   39
#define LCD_CS     21
#define LCD_DC     45
#define LCD_RST    47
#define LCD_BL     40
#define LCD_H_RES  240
#define LCD_V_RES  320

static const char *TAG = "lcd_driver";
static esp_lcd_panel_handle_t panel_handle;

static void gb_swap_buf(uint16_t *pixels, int count)
{
    for (int i = 0; i < count; i++) {
        uint16_t c = pixels[i];
        uint16_t r_out = (c & 0x001F) << 11;
        uint16_t g_out = (c & 0x07E0);
        uint16_t b_out = (c & 0xF800) >> 11;
        pixels[i] = __builtin_bswap16(r_out | g_out | b_out);
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    gb_swap_buf((uint16_t *)px_map, w * h);
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x1 + w, area->y1 + h, px_map);
    lv_display_flush_ready(disp);
}

#define LVGL_BUF_LINES 40

static lv_color_t lvgl_buf1[LCD_H_RES * LVGL_BUF_LINES];
static lv_color_t lvgl_buf2[LCD_H_RES * LVGL_BUF_LINES];

void lcd_lvgl_init(void)
{
    lcd_driver_init();

    lv_init();

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, lvgl_buf1, lvgl_buf2,
                           sizeof(lvgl_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
}

esp_err_t lcd_driver_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = LCD_MISO,
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

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

void lcd_draw_bitmap(uint16_t *pixels, int x, int y, int w, int h)
{
    gb_swap_buf(pixels, w * h);
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, pixels);
}

/*============================================================================
 * Touch Input
 *============================================================================*/

static void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    ft6336_touch_data_t touch;
    ft6336_read(&touch);

    if (touch.num_touches > 0) {
        data->point.x = touch.points[0].x;
        data->point.y = touch.points[0].y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void lcd_lvgl_touch_init(void)
{
    esp_err_t ret = ft6336_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed (%d), skipping indev", ret);
        return;
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touchpad_read);

    ESP_LOGI(TAG, "LVGL touch indev registered");
}
