#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "lvgl.h"

static const char *TAG = "app_main";

static void lvgl_task(void *arg)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    lcd_lvgl_init();
    ESP_LOGI(TAG, "LVGL display initialized");

    esp_err_t ret = ft6336_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 init failed");
    }

    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Touch Test");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    xTaskCreate(lvgl_task, "lvgl", 16384, NULL, 1, NULL);

    while (1) {
        ft6336_touch_data_t touch;
        ft6336_read(&touch);
        if (touch.num_touches > 0) {
            int16_t y_flip = 319 - touch.points[0].y;
            ESP_LOGI(TAG, "TOUCH raw=(%d,%d) flip=(%d,%d) points=%d",
                     touch.points[0].x, touch.points[0].y,
                     touch.points[0].x, y_flip,
                     touch.num_touches);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
