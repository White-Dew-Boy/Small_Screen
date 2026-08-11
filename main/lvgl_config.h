#pragma once

/* Tell LVGL to use this file as config */
#define LV_CONF_INCLUDE_SIMPLE

/* Display */
#define LV_COLOR_DEPTH          16
#define LV_COLOR_16_SWAP         1   /* ESP32-S3 + ILI9341 needs byte swap */

/* Memory */
#define LV_MEM_SIZE             (48 * 1024)
#define LV_DEF_REFR_PERIOD       33  /* ~30 FPS */

/* OS / Tick — use FreeRTOS */
#define LV_TICK_CUSTOM           1
#define LV_TICK_CUSTOM_INCLUDE  "freertos/FreeRTOS.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR  (xTaskGetTickCount() * portTICK_PERIOD_MS)

/* Features — minimal for demo */
#define LV_USE_LOG               0
#define LV_USE_ASSERT_NULL       1
#define LV_USE_ASSERT_MALLOC     1
#define LV_USE_PERF_MONITOR      0
#define LV_USE_MEM_MONITOR       0
#define LV_USE_SYSMON            0
#define LV_USE_PROFILER          0
#define LV_USE_MONKEY            0
#define LV_USE_DRAW_SW           1
#define LV_DRAW_SW_SUPPORT_RGB565 1

/* Widgets — keep all enabled */
#define LV_USE_BTN               1
#define LV_USE_LABEL             1
#define LV_USE_SLIDER            1
#define LV_USE_ARC               1

/* Fonts */
#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_20    1
#define LV_FONT_DEFAULT          1
