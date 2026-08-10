#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_driver.h"
#include "lvgl.h"

static const char *TAG = "app_main";

static void lvgl_task(void *arg)
{
    while (1) {
        lcd_lvgl_touch_poll();
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/*----------------------------------------------------------------------------
 * Event callbacks
 *----------------------------------------------------------------------------*/
static void btn_clicked_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Touch button pressed!");
}

/*----------------------------------------------------------------------------
 * Application entry
 *----------------------------------------------------------------------------*/
void app_main(void)
{
    lcd_lvgl_init();
    ESP_LOGI(TAG, "LVGL display initialized");
    lcd_lvgl_touch_init();

    /* ---- Simple touch demo UI ---- */
    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "Touch Demo");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* A button to test touch-responding widget */
    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, btn_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Tap Me");
    lv_obj_center(btn_label);

    /* A slider for drag testing */
    lv_obj_t *slider = lv_slider_create(lv_screen_active());
    lv_obj_set_width(slider, 200);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -20);

    ESP_LOGI(TAG, "Touch demo ready — tap the button or drag the slider");

    xTaskCreate(lvgl_task, "lvgl", 16384, NULL, 1, NULL);
    vTaskDelete(NULL);
}
