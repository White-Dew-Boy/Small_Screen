#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "lvgl.h"

static const char *TAG = "app_main";
static lv_obj_t *coord_label;

static void update_coord_label(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        lv_label_set_text(coord_label, "X: ---  Y: ---");
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_indev_state_t state = lv_indev_get_state(indev);
    if (state == LV_INDEV_STATE_PRESSED) {
        lv_label_set_text_fmt(coord_label, "X: %4d  Y: %4d", point.x, point.y);
    } else {
        lv_label_set_text(coord_label, "X: ---  Y: ---");
    }
}

static void lvgl_task(void *arg)
{
    uint32_t count = 0;
    while (1) {
        lcd_lvgl_touch_poll();
        lv_timer_handler();
        if (++count % 10 == 0) {
            update_coord_label();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void btn_clicked_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Touch button pressed!");
}

void app_main(void)
{
    lcd_lvgl_init();
    ESP_LOGI(TAG, "LVGL display initialized");
    lcd_lvgl_touch_init();

    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "Touch Demo");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    coord_label = lv_label_create(lv_screen_active());
    lv_label_set_text(coord_label, "X: ---  Y: ---");
    lv_obj_align(coord_label, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Tap Me");
    lv_obj_center(btn_label);

    lv_obj_t *slider = lv_slider_create(lv_screen_active());
    lv_obj_set_width(slider, 200);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -20);

    ESP_LOGI(TAG, "Touch demo ready");

    xTaskCreate(lvgl_task, "lvgl", 16384, NULL, 1, NULL);
    vTaskDelete(NULL);
}
