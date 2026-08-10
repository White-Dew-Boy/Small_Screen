#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "ft6336.h"

#define LCD_W  240
#define LCD_H  320

static const char *TAG = "app_main";

static uint16_t *fb;

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y; row < y + h; row++) {
        for (int col = x; col < x + w; col++) {
            if (col >= 0 && col < LCD_W && row >= 0 && row < LCD_H) {
                fb[row * LCD_W + col] = color;
            }
        }
    }
}

void app_main(void)
{
    fb = malloc(LCD_W * LCD_H * sizeof(uint16_t));
    assert(fb);

    lcd_driver_init();
    ESP_LOGI(TAG, "LCD ready");

    ft6336_init();
    ESP_LOGI(TAG, "Touch ready");

    while (1) {
        ft6336_touch_data_t touch;
        ft6336_read(&touch);

        memset(fb, 0, LCD_W * LCD_H * sizeof(uint16_t));

        if (touch.num_touches > 0) {
            int16_t tx = touch.points[0].x;
            int16_t ty = LCD_H - 1 - touch.points[0].y;

            fill_rect(tx - 20, ty - 2, 40, 4, 0xF800);
            fill_rect(tx - 2, ty - 20, 4, 40, 0xF800);

            ESP_LOGI(TAG, "TOUCH raw=(%d,%d) flip=(%d,%d)",
                     touch.points[0].x, touch.points[0].y, tx, ty);
        }

        lcd_draw_bitmap(fb, 0, 0, LCD_W, LCD_H);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
