#pragma once
#include "lvgl.h"

/**
 * @brief Create the PC Config page (reached from the PC Performance page's
 *        top-right "Config" button).
 *
 *        Shows:
 *          - live link state of BOTH transports (MQTT broker/PC, BLE
 *            peripheral/PC), refreshed by an internal LVGL timer while the
 *            page is on screen;
 *          - the data-source choice: MQTT (default) / BLE / Off. Tapping
 *            an option saves it immediately (NVS) and returns to the PC
 *            Performance page.
 *
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_pc_perf_cfg_create(void);

/**
 * @brief Register the callback invoked when leaving the page (option picked
 *        or Back pressed). The callback must switch back to the PC
 *        Performance page from the LVGL thread.
 */
void ui_pc_perf_cfg_set_back_cb(void (*cb)(void));
