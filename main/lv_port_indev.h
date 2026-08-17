#pragma once
#include "esp_err.h"
#include "lvgl.h"
esp_err_t lv_port_indev_init(void);
lv_indev_t *lv_port_indev_get(void);
