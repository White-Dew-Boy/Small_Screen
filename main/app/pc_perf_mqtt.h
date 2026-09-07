#pragma once
#include "esp_err.h"
#include <stdbool.h>

#include "ble_perf.h" /* pc_perf_data_t: shared snapshot type */

/**
 * @brief PC performance data over MQTT — alternate transport to the BLE
 *        peripheral in app/ble_perf.c.
 *
 * The PC publishes a flat JSON object to the configured topic (default
 * "pc/performance", QoS 1, see CONFIG_MQTT_PC_PERF_TOPIC):
 *   {"timestamp":..,"cpu":..,"memory":..,
 *    "upload_speed":..,"download_speed":..,"gpu":..,"disk":..}
 *   cpu/memory/gpu/disk in percent, upload_speed/download_speed in KB/s.
 *   "timestamp" is wall-clock seconds and informational only — this driver
 *   stamps the reception time itself (same as ble_perf).
 *
 * Data validity follows materials/mqtt_payload.md: a KEY PRESENT in the
 * JSON means the field has data (0 is a legal measured value, e.g. GPU
 * idle 0%); a MISSING key means not collected and the UI hides the row /
 * shows "--". temp_c/fps keys are never sent -> their presence bits never
 * get set.
 *
 * Parsed values are published into the SAME pc_perf_data_t snapshot type
 * used by the BLE driver so the UI page can display either transport.
 */

/**
 * @brief Register the pc/performance subscription with mqtt_manager.
 *
 * Safe to call right after mqtt_manager_init(); the actual broker
 * subscription is (re)issued by the manager whenever the client connects.
 * When the configured topic is empty the MQTT path is disabled (returns
 * ESP_OK, nothing registered).
 *
 * @return ESP_OK on success, otherwise an error code (non-fatal).
 */
esp_err_t pc_perf_mqtt_init(void);

/**
 * @brief Copy the latest PC performance snapshot received over MQTT.
 *        `connected` reflects the broker link at call time.
 *        Safe to call from any task.
 */
void pc_perf_mqtt_get_data(pc_perf_data_t *out);

/**
 * @brief True once at least one valid PC frame has been received over MQTT
 *        since boot.
 */
bool pc_perf_mqtt_has_data(void);
