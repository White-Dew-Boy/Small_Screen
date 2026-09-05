#pragma once
#include "lvgl.h"

/**
 * @brief Create the PC Performance page. Shows the metrics streamed from a
 *        PC over the transport selected on the Config page (top-right
 *        "Config" button -> ui_pc_perf_cfg; default MQTT): MQTT topic
 *        pc/performance (see drivers/pc_perf_mqtt.h) or the BLE peripheral
 *        (drivers/ble_perf.h). CPU / memory usage bars plus network
 *        upload/download speeds; GPU, disk, temperature and FPS rows appear
 *        automatically when the sender reports them. Link/connection state
 *        is shown on the Config page, not here.
 *
 *        Values refresh via an internal LVGL timer while the page is on
 *        screen. In BLE mode the NimBLE peripheral starts lazily the first
 *        time this page is shown (internal RAM is too tight at boot).
 *
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_pc_perf_create(void);

/**
 * @brief Register the callback invoked by the top-right "Config" button.
 *        The callback must switch to the Config page (ui_pc_perf_cfg) from
 *        the LVGL thread.
 */
void ui_pc_perf_set_cfg_cb(void (*cb)(void));
