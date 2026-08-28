#pragma once
#include "lvgl.h"

/**
 * @brief Create the SD picture slideshow page (gallery).
 *
 * Scans the upload directory (/sdcard/esp32_files) for 240x320 raw RGB565
 * .bin images (153,600 bytes each, produced by tools/rgb565_convert.py
 * with --raw) and auto-advances through them every 10 seconds, full
 * screen, with only a small file-name/index overlay in the top-left
 * corner. Leave the page with KEY3 (returns to the Home menu) or KEY2.
 *
 * All SD reads happen on the LVGL task (shared SPI bus, see sd_card.h);
 * each frame load blocks the task for ~50-100 ms while the previous frame
 * stays on screen.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_gallery_create(void);

/**
 * @brief Re-scan the upload directory for pictures and show the first one.
 *        Call (from the LVGL thread) right before the page is shown.
 */
void ui_gallery_refresh(void);
