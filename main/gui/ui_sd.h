#pragma once
#include "lvgl.h"

/**
 * @brief Create the SD card management page. Shows whether an SD card is
 *        connected and, when it is, basic card info (name, capacity).
 *
 *        A background task inside this module polls the card once per
 *        second. The check issues CMD13, which may block up to ~1 s when
 *        the card is missing, so it must never run on the LVGL thread.
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sd_create(void);

/**
 * @brief Register a callback invoked when the "Browse Files" button is
 *        pressed. The callback must switch to the file-browser page from
 *        the LVGL thread.
 */
void ui_sd_set_browse_cb(void (*cb)(void));
