#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "sd_card.h"
#include "rgb_led.h"
#include "gpa.h"
#include "esp_heap_caps.h"
#include <inttypes.h>

static const char *TAG = "app_main";

#define KEY1_GPIO  GPIO_NUM_4

#define RGB_LED_BRIGHTNESS  255

static void hsv_to_rgb(uint16_t hue, uint8_t sat, uint8_t val,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (sat == 0) {
        *r = *g = *b = val;
        return;
    }

    uint8_t region = (uint8_t)(hue / 60);
    uint8_t rem = (uint8_t)(hue % 60);
    uint8_t p = (uint8_t)((uint16_t)val * (255 - sat) / 255);
    uint8_t q = (uint8_t)((uint16_t)val * (255 - (uint16_t)sat * rem / 60) / 255);
    uint8_t t = (uint8_t)((uint16_t)val * (255 - (uint16_t)sat * (60 - rem) / 60) / 255);

    switch (region) {
        case 0: *r = val; *g = t;   *b = p;   break;
        case 1: *r = q;   *g = val; *b = p;   break;
        case 2: *r = p;   *g = val; *b = t;   break;
        case 3: *r = p;   *g = q;   *b = val; break;
        case 4: *r = t;   *g = p;   *b = val; break;
        default: *r = val; *g = p;  *b = q;   break;
    }
}

static void rgb_led_anim_task(void *arg)
{
    uint16_t hue = 0;
    while (1) {
        uint8_t r, g, b;
        hsv_to_rgb(hue, 255, RGB_LED_BRIGHTNESS, &r, &g, &b);
        rgb_led_set_all(r, g, b);
        rgb_led_refresh();
        hue = (hue + 1) % 360;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    gpio_config_t key1_cfg = {
        .pin_bit_mask = BIT64(KEY1_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&key1_cfg);
    gpio_set_level(KEY1_GPIO, 1);

    ESP_ERROR_CHECK(gpa_init());
    ESP_ERROR_CHECK(gpa_power_on(GPA_ID_LCD));

    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_init();
    ft6336_init();

    ESP_ERROR_CHECK(rgb_led_init());
    xTaskCreate(rgb_led_anim_task, "rgb_anim", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Free heap: internal=%" PRIu32 " KB, PSRAM=%" PRIu32 " KB",
             esp_get_free_internal_heap_size() / 1024,
             esp_get_free_heap_size() / 1024);

    ESP_LOGI(TAG, "Ready");

    int16_t last_cx = -1, last_cy = -1;

    while (1) {
        ft6336_point_t pt;
        if (!ft6336_wait_touch(&pt)) continue;

        int16_t tx = 239 - pt.x;
        int16_t ty = 319 - pt.y;

        while (1) {
            ft6336_touch_data_t touch;
            ft6336_read(&touch);

            if (touch.num_touches > 0) {
                tx = 239 - touch.points[0].x;
                ty = 319 - touch.points[0].y;

                lcd_move_cross(last_cx, last_cy, tx, ty);
                last_cx = tx;
                last_cy = ty;
            } else {
                static int zero_count;
                if (++zero_count >= 5) {
                    zero_count = 0;
                    break;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        lcd_clear_cross(last_cx, last_cy);
        last_cx = last_cy = -1;
        ft6336_enter_monitor();
    }
}
