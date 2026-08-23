#pragma once
#include "lvgl.h"

/**
 * @brief Create the custom RGB color page: three sliders (R/G/B, 0..255)
 *        applied to the target selected on the LED control page in real
 *        time.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_led_custom_create(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the LED control page from the
 *        LVGL thread.
 */
void ui_led_custom_set_back_cb(void (*cb)(void));

/**
 * @brief Re-read the LED page's selected target color and move the R/G/B
 *        sliders to it. Call every time the page is entered (the sliders
 *        are created once and would otherwise keep stale values).
 */
void ui_led_custom_refresh(void);
