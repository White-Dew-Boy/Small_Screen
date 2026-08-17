#include "lvgl.h"
#include "ft6336.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static lv_indev_t *s_indev;
static lv_indev_drv_t indev_drv;
static bool task_bound;
static bool last_pressed;

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;

    /* Bind the ISR notification to the task that runs this read_cb
     * (the LVGL task) on first invocation. */
    if (!task_bound) {
        ft6336_bind_task(xTaskGetCurrentTaskHandle());
        task_bound = true;
    }

    ft6336_touch_data_t touch;

    if (last_pressed) {
        /* Finger held: INT only fires once per touch-down, so keep
         * reading registers until the finger lifts. */
        ft6336_read(&touch);
    } else {
        /* Idle: sleep on the INT notification — zero I2C traffic.
         * 30ms timeout matches the LVGL read period (fallback if an
         * interrupt were ever missed). */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30)) == 0) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(15));  /* debounce, mirrors wait_touch */
        ft6336_read(&touch);
        int retry = 3;
        while (touch.num_touches == 0 && --retry > 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            ft6336_read(&touch);
        }
    }

    last_pressed = (touch.num_touches > 0);
    if (touch.num_touches > 0) {
        data->point.x = touch.points[0].x;
        data->point.y = touch.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t lv_port_indev_init(void)
{
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    s_indev = lv_indev_drv_register(&indev_drv);
    return ESP_OK;
}

lv_indev_t *lv_port_indev_get(void)
{
    return s_indev;
}
