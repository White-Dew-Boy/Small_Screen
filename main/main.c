#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "sd_card.h"

static const char *TAG = "app_main";

void app_main(void)
{
    /* SD must init first — enters SPI mode before LCD communicates on shared bus */
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_init();
    ft6336_init();
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
