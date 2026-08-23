#pragma once
#include "lvgl.h"

/**
 * @brief Create the "Nearby WiFi" page: auto-scans and lists nearby APs.
 *        Tapping one asks for its password (show/hide supported), then
 *        Confirm saves and connects. Back returns to the list / status page.
 *        Must be called after lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_nearby_wifi_create(void);

/**
 * @brief Register a callback invoked when leaving the page (Back from the
 *        list view, or Confirm after entering a password). The callback
 *        must switch back to the WiFi status page from the LVGL thread.
 */
void ui_nearby_wifi_set_back_cb(void (*cb)(void));

/**
 * @brief Start a fresh scan. Call every time the page is entered.
 *        Non-blocking; the list fills in once the scan completes.
 */
void ui_nearby_wifi_start_scan(void);
