#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the MQTT manager.
 *
 * Builds the device id (from the WiFi MAC) and topic names, and starts a
 * background timer that watches the WiFi state: as soon as WiFi is
 * connected, the MQTT client is created and started. Reconnects after
 * WiFi drops are handled by esp-mqtt internally.
 *
 * Non-fatal: if the broker is unreachable, the device keeps working and
 * retries in the background.
 *
 * @return ESP_OK on success.
 */
esp_err_t mqtt_manager_init(void);

/**
 * @brief Check whether the MQTT client is currently connected to the broker.
 * @return true if connected.
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Publish temperature/humidity telemetry.
 *
 * Rate-limited to CONFIG_MQTT_TELEMETRY_INTERVAL. No-op while
 * disconnected. Payload (JSON): {"temp":..,"humi":..,"rssi":..,"ip":..}.
 *
 * Thread-safe; may be called from any task.
 *
 * @param[in] temp  Temperature in degrees Celsius.
 * @param[in] humi  Relative humidity in percent.
 */
void mqtt_manager_publish_telemetry(float temp, float humi);

/**
 * @brief Get the device id used in topics, e.g. "esp32s3_1a2b3c".
 * @return Pointer to the static id string.
 */
const char *mqtt_manager_get_device_id(void);

/**
 * @brief Get the telemetry topic, e.g. "devices/esp32s3_1a2b3c/telemetry".
 * @return Pointer to the static topic string.
 */
const char *mqtt_manager_get_telemetry_topic(void);

/**
 * @brief Get the number of telemetry messages actually published.
 * @return Publish counter.
 */
uint32_t mqtt_manager_get_publish_count(void);

#ifdef __cplusplus
}
#endif
