#pragma once
#include "lvgl.h"

/**
 * @brief Create the SD card management page. Shows whether an SD card is
 *        connected, card info (name, capacity), a button to open the file
 *        browser and a button that toggles the HTTP upload/download server
 *        (files land flat in /sdcard/esp32_files, duplicates numbered).
 *
 *        A 1 s LVGL timer probes/mounts the card while the page is visible
 *        (may block up to ~1 s when the card is missing — acceptable here)
 *        and a 10 ms LVGL timer executes the upload server's SD file I/O.
 *        All SD traffic stays on the LVGL task (shared SPI bus, see
 *        sd_card.h / lcd_driver.h).
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_sd_create(void);

/**
 * @brief Register a callback invoked when the "Browse Files" button is
 *        pressed. The callback must switch to the file-browser page from
 *        the LVGL thread.
 */
void ui_sd_set_browse_cb(void (*cb)(void));
