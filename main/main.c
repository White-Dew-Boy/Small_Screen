#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "ili9341_lvgl.h"
#include "ft6336_lvgl.h"
#include "lvgl.h"

static const char *TAG = "app_main";

#define LVGL_TASK_STACK  8192
#define LVGL_TASK_PRIO   1

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 5) delay_ms = 5;
        if (delay_ms == 0) delay_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void lvgl_demo_ui(void)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello LVGL!");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click Me");
    lv_obj_center(btn_label);

    lv_obj_t *arc = lv_arc_create(lv_screen_active());
    lv_obj_set_size(arc, 150, 150);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 100);
    lv_arc_set_value(arc, 60);
}

void app_main(void)
{
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_driver_init();
    lv_init();
    ili9341_lvgl_init();
    ft6336_lvgl_init();
    ft6336_init();

    lvgl_demo_ui();

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL,
                             LVGL_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "Ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
