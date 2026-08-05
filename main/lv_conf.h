#pragma once

#define LV_CONF_INCLUDE_SIMPLE

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_DPI_DEF 130

#define LV_DRAW_BUF_STRIDE_ALIGN 1

#define LV_USE_LOG 0

#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_RGB565 1

#define LV_USE_OBSERVER 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM
#define LV_TICK_CUSTOM_INCLUDE "freertos/FreeRTOS.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (pdTICKS_TO_MS(xTaskGetTickCount()))
#endif

#define LV_MEM_SIZE (48 * 1024)
