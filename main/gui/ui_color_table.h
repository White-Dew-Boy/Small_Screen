#pragma once
#include "lvgl.h"

/**
 * @brief Create the color table UI (a grid of swatches; tap a swatch to see
 *        its name and hex code in the bottom info bar).
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_color_table_create(void);
