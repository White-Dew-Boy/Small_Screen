#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Data Structures
 *============================================================================*/
/* Runtime MQTT connection configuration (editable from the UI, persisted
 * in NVS). Empty username/password means anonymous. */
typedef struct {
    char uri[160];        /* Full broker URI, e.g. "wss://host:443/mqtt" */
    char username[33];    /* Empty string = anonymous */
    char password[65];    /* Empty string = anonymous */
    char client_id[48];   /* Empty string = auto-generated */
    int keepalive_s;      /* Keepalive in seconds, 0 = default (120) */
} mqtt_cfg_t;

/* One entry of the recent-command ring buffer. */
typedef struct {
    char text[64]; /* Command payload (truncated) */
} mqtt_cmd_entry_t;

/* Number of commands kept in the history ring buffer. */
#define MQTT_CMD_HISTORY_MAX 8

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize the MQTT manager.
 *
 * Loads the runtime config from NVS (falling back to Kconfig defaults),
 * builds the device id and topic names, and starts a background timer that
 * watches the WiFi state: as soon as WiFi is connected, the MQTT client is
 * created and started. Reconnects after WiFi drops are handled by esp-mqtt
 * internally.
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
 * Rate-limited to the configured interval. No-op while disconnected.
 * Payload (JSON): {"temp":..,"humi":..,"rssi":..,"ip":..}.
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
 * @brief Get the retained status topic, e.g. "devices/esp32s3_1a2b3c/status".
 * @return Pointer to the static topic string.
 */
const char *mqtt_manager_get_status_topic(void);

/**
 * @brief Get the command (subscribed) topic, e.g. "devices/esp32s3_1a2b3c/cmd".
 * @return Pointer to the static topic string.
 */
const char *mqtt_manager_get_cmd_topic(void);

/**
 * @brief Get the command-response topic, e.g. "devices/esp32s3_1a2b3c/cmd_resp".
 * @return Pointer to the static topic string.
 */
const char *mqtt_manager_get_cmd_resp_topic(void);

/**
 * @brief Get the number of telemetry messages actually published.
 * @return Publish counter.
 */
uint32_t mqtt_manager_get_publish_count(void);

/*============================================================================
 * Runtime configuration
 *============================================================================*/
/**
 * @brief Copy the current MQTT configuration.
 * @param[out] out  Receives the current config.
 */
void mqtt_manager_get_cfg(mqtt_cfg_t *out);

/**
 * @brief Apply a new MQTT configuration.
 *
 * Persists the config to NVS and restarts the MQTT client with the new
 * settings (teardown + recreate) so the change takes effect immediately.
 *
 * @param[in] cfg  New configuration.
 * @return ESP_OK on success.
 */
esp_err_t mqtt_manager_set_cfg(const mqtt_cfg_t *cfg);

/**
 * @brief Get the current telemetry publish interval in seconds.
 * @return Interval in seconds.
 */
int mqtt_manager_get_interval(void);

/**
 * @brief Set the telemetry publish interval at runtime (1..3600 s).
 * @param[in] seconds  New interval.
 * @return ESP_OK on success.
 */
esp_err_t mqtt_manager_set_interval(int seconds);

/*============================================================================
 * Connection control
 *============================================================================*/
/**
 * @brief Disconnect from the broker (stops the MQTT client).
 * @return ESP_OK on success.
 */
esp_err_t mqtt_manager_disconnect(void);

/**
 * @brief (Re)connect to the broker. No-op if already connected.
 * @return ESP_OK if a connect was started.
 */
esp_err_t mqtt_manager_connect(void);

/*============================================================================
 * Command history
 *============================================================================*/
/**
 * @brief Get the recent command history (oldest first).
 * @param[out] out    Buffer for up to `max` entries.
 * @param[in]  max    Capacity of the buffer (use MQTT_CMD_HISTORY_MAX).
 * @return Number of entries copied.
 */
size_t mqtt_manager_get_cmd_history(mqtt_cmd_entry_t *out, size_t max);

#ifdef __cplusplus
}
#endif
