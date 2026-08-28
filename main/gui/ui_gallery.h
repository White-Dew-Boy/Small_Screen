#pragma once
#include "lvgl.h"

/**
 * @brief Create the SD picture slideshow page (gallery).
 *
 * Scans the upload directory (/sdcard/esp32_files) for 240x320 raw RGB565
 * .bin images (153,600 bytes each, produced by tools/rgb565_convert.py
 * with --raw), auto-advances every few seconds, and lets the user flip
 * through them by tapping the left/right half of the screen.
 *
 * All SD reads happen on the LVGL task (shared SPI bus, see sd_card.h);
 * each frame load blocks the task for ~50-100 ms, preceded by a rendered
 * "Loading" label (lv_refr_now) so the UI never appears frozen.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_gallery_create(void);

/**
 * @brief Re-scan the upload directory for pictures and show the first one.
 *        Call (from the LVGL thread) right before the page is shown.
 */
void ui_gallery_refresh(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the parent page from the LVGL
 *        thread.
 */
void ui_gallery_set_back_cb(void (*cb)(void));
