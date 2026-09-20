#pragma once
#include "lvgl.h"

/**
 * @brief Create the audio player page: the .wav files in CONFIG_UPLOAD_DIR
 *        (/sdcard/esp32_files), tapped to start/stop playback, plus volume
 *        and stop/back buttons.
 *
 *        The page is deliberately refresh-free: nothing is updated on a
 *        timer — no progress bar, no timecode, no animation. Redrawing the
 *        screen while audio plays competes with the SD reads that keep the
 *        I2S ring fed, which is audible as crackle and slows the UI down.
 *        The only screen updates are the ones a tap causes, plus one redraw
 *        when the track changes or ends by itself (state-change check, not a
 *        periodic redraw).
 *
 *        All SD access happens on the LVGL task (shared SPI bus with the
 *        LCD), same as every other page.
 *
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_player_create(void);

/**
 * @brief Re-scan CONFIG_UPLOAD_DIR for .wav files and rebuild the list.
 *        Call when entering the page (blocks briefly while reading the
 *        directory over SPI).
 */
void ui_player_refresh(void);

/**
 * @brief Register the callback invoked by the "Back" button. The callback
 *        must switch back to the SD card page from the LVGL thread.
 */
void ui_player_set_back_cb(void (*cb)(void));
