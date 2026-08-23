#pragma once
#include "lvgl.h"

/**
 * @brief Create the preset color selection page. Tapping a color applies
 *        it to the target selected on the LED control page and returns.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_led_preset_create(void);

/**
 * @brief Register a callback invoked when leaving the page (color picked
 *        or Back). The callback must switch back to the LED control page
 *        from the LVGL thread.
 */
void ui_led_preset_set_back_cb(void (*cb)(void));
