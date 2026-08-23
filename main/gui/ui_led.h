#pragma once
#include "lvgl.h"
#include <stdint.h>

/**
 * @brief Create the RGB LED control page: pick a target (LED1/LED2/LED3/
 *        All), set brightness with a slider, and open the preset / custom
 *        color sub-pages.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_led_create(void);

/**
 * @brief Register a callback invoked when the "Preset Colors" button is
 *        pressed. The callback must switch to the preset color page from
 *        the LVGL thread.
 */
void ui_led_set_preset_cb(void (*cb)(void));

/**
 * @brief Register a callback invoked when the "Custom RGB" button is
 *        pressed. The callback must switch to the custom color page from
 *        the LVGL thread.
 */
void ui_led_set_custom_cb(void (*cb)(void));

/*============================================================================
 * Shared LED state access (used by the sub-pages)
 *============================================================================*/
/** @brief Current target selection: 0..2 = LED index, 3 = all. */
int ui_led_get_target(void);

/**
 * @brief Apply a preset color to the current target.
 * @param[in] idx  Index into the preset color table.
 */
void ui_led_apply_preset(int idx);

/**
 * @brief Apply a custom RGB color to the current target.
 */
void ui_led_apply_rgb(int r, int g, int b);

/**
 * @brief Get the current target's color (all -> LED1's color).
 */
void ui_led_get_rgb(int *r, int *g, int *b);
