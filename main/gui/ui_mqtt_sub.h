#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the MQTT subscribe page: shows the command topic the
 *        device subscribes to and the supported command payloads.
 *
 * Must be called after lv_init() and lv_port_disp_init().
 *
 * @return The new screen object.
 */
lv_obj_t *ui_mqtt_sub_create(void);

/**
 * @brief Set the callback invoked by the Back button.
 *
 * The callback must switch back to the MQTT status page from the
 * LVGL thread (called from an LVGL event).
 *
 * @param[in] cb  Callback, or NULL to disable the Back button.
 */
void ui_mqtt_sub_set_back_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
