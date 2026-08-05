#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "lvgl.h"

static const char *TAG = "app_main";

void app_main(void)
{
    lcd_lvgl_init();
    ESP_LOGI(TAG, "LVGL initialized");

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "Hello LVGL!");
    lv_obj_set_style_text_color(label, lv_color_hex(0x0000FF), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
