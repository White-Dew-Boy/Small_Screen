#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lcd_driver.h"
#include "ft6336.h"
#include "sd_card.h"
#include "power.h"

static const char *TAG = "app_main";

static lv_obj_t *touch_dot;
static lv_obj_t *count_label;
static uint32_t click_count;

static void lvgl_task(void *arg)
{
    (void)arg;
    bool hwm_logged = false;
    while (1) {
        if (!hwm_logged) {
            hwm_logged = true;
            ESP_LOGI(TAG, "LVGL task stack high water: %lu",
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void move_dot_cb(lv_timer_t *timer)
{
    (void)timer;
    lv_indev_t *indev = lv_port_indev_get();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(touch_dot, p.x - 10, p.y - 10);
}

static void btn_click_cb(lv_event_t *e)
{
    (void)e;
    click_count++;
    lv_label_set_text_fmt(count_label, "Tap: %lu", (unsigned long)click_count);
}

static void ui_demo_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "LVGL 8.3.11 OK");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 160, 48);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, NULL);

    count_label = lv_label_create(btn);
    lv_label_set_text(count_label, "Tap: 0");
    lv_obj_center(count_label);

    touch_dot = lv_obj_create(scr);
    lv_obj_set_size(touch_dot, 20, 20);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(touch_dot, 0, 0);

    lv_timer_create(move_dot_cb, 30, NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(power_on(POWER_ID_LCD));

    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s), continuing without SD",
                 esp_err_to_name(sd_ret));
    }

    lcd_init();
    ft6336_init();

    lv_init();
    ESP_ERROR_CHECK(lv_port_tick_init());
    ESP_ERROR_CHECK(lv_port_disp_init());
    ESP_ERROR_CHECK(lv_port_indev_init());

    ui_demo_create();

    ESP_LOGI(TAG, "Free heap: internal=%lu KB, PSRAM=%lu KB",
             (unsigned long)esp_get_free_internal_heap_size() / 1024,
             (unsigned long)esp_get_free_heap_size() / 1024);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 7168, NULL, 5, NULL, 0);
}
