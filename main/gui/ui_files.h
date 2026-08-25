#pragma once
#include "lvgl.h"

/**
 * @brief Create the SD card file-browser page (file-manager style).
 *
 * Shows one directory level at a time in a scrollable list: directories
 * first, then files. Tapping a directory enters it, the ".." entry (or the
 * Back button) goes up / leaves the page. Tapping a file shows its name
 * and size in the info bar.
 *
 * All SD I/O happens inside this module's refresh path, which runs on the
 * LVGL task (the SD card shares the SPI bus with the LCD — see sd_card.h).
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_files_create(void);

/**
 * @brief Re-read the current directory and rebuild the list. Must be called
 *        (from the LVGL thread) right before the page is shown, and is also
 *        invoked internally on every navigation.
 */
void ui_files_refresh(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the parent page from the LVGL
 *        thread.
 */
void ui_files_set_back_cb(void (*cb)(void));
