#include "ft6336_lvgl.h"

#include "ft6336.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "ft6336_lvgl";

/*============================================================================
 * Touch read callback — LVGL calls this periodically to poll touch state
 *============================================================================*/
static void ft6336_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ft6336_touch_data_t touch;
    ft6336_read(&touch);

    if (touch.num_touches > 0) {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = touch.points[0].x;
        data->point.y = touch.points[0].y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*============================================================================
 * Public API
 *============================================================================*/
esp_err_t ft6336_lvgl_init(void)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, ft6336_read_cb);

    ESP_LOGI(TAG, "LVGL touch input registered (FT6336)");
    return ESP_OK;
}
