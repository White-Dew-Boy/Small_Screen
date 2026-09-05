#pragma once
#include "lvgl.h"

/**
 * @brief Create the "Saved WiFi" page: lists saved networks (max 5).
 *        Tapping a name selects (highlights) it; the bottom Connect and
 *        Delete buttons are grayed out until a network is selected.
 *        Connect starts a connection attempt; Delete removes the network
 *        from NVS after a confirmation dialog. Must be called after
 *        lv_init() and lv_port_disp_init().
 * @return The created screen object (not loaded yet — use lv_scr_load()).
 */
lv_obj_t *ui_saved_wifi_create(void);

/**
 * @brief Register a callback invoked when the Back button is pressed.
 *        The callback must switch back to the WiFi status page from the
 *        LVGL thread.
 */
void ui_saved_wifi_set_back_cb(void (*cb)(void));

/**
 * @brief Rebuild the saved-network list. Call when the page is entered
 *        so newly saved networks show up.
 */
void ui_saved_wifi_refresh(void);
