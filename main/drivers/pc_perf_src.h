#pragma once
#include "esp_err.h"

/**
 * @brief Data-source selection for the PC-Perf page (see ui_pc_perf.c).
 *
 * Exactly one transport is shown; the choice is persisted in NVS and can be
 * changed from the page's settings sub-page (ui_pc_perf_cfg). Default MQTT.
 */
typedef enum {
    PC_PERF_SRC_MQTT = 0, /* subscribe topic pc/performance (default) */
    PC_PERF_SRC_BLE  = 1, /* BLE peripheral "ESP32_PC_Monitor" */
    PC_PERF_SRC_OFF  = 2, /* do not display PC data */
} pc_perf_src_t;

/**
 * @brief Get the configured data source (NVS-backed, default MQTT).
 * @return Current source.
 */
pc_perf_src_t pc_perf_src_get(void);

/**
 * @brief Set and persist the data source.
 * @param[in] src New source.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad value,
 *         or an NVS error.
 */
esp_err_t pc_perf_src_set(pc_perf_src_t src);
