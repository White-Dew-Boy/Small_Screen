#pragma once
#include "lvgl.h"

/**
 * @brief Create the PC Performance page. Shows the metrics streamed from a
 *        PC over BLE (see drivers/ble_perf.h for the JSON wire protocol):
 *        CPU / memory usage bars plus network upload/download speeds; GPU,
 *        disk, temperature and FPS rows appear automatically when the
 *        sender reports them. A connection status line is on top.
 *
 *        BLE starts lazily the first time this page is shown (internal RAM
 *        is too tight to run NimBLE at boot). Values refresh via an
 *        internal LVGL timer while the page is on screen.
 *
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_pc_perf_create(void);
