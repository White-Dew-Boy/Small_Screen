#include "lvgl.h"
#include "ft6336.h"

static lv_indev_t *s_indev;
static lv_indev_drv_t indev_drv;

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    ft6336_touch_data_t touch;
    ft6336_read(&touch);
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
