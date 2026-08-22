#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Data Structures
 *============================================================================*/
typedef enum {
    WIFI_STATE_IDLE = 0,     /* Not initialized */
    WIFI_STATE_DISCONNECTED, /* Init done, not connected */
    WIFI_STATE_CONNECTING,   /* Connecting / reconnecting */
    WIFI_STATE_CONNECTED,    /* Got IP address */
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    char ssid[33];       /* Connected SSID */
    char ip[16];         /* Assigned IPv4 address ("" if none) */
    int8_t rssi;         /* Signal strength in dBm (0 if unknown) */
    uint32_t reconnect_cnt; /* Number of automatic reconnects performed */
} wifi_info_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize NVS, netif, event loop and WiFi in STA mode,
 *        then start connecting to the AP configured via Kconfig
 *        (CONFIG_WIFI_SSID / CONFIG_WIFI_PASSWORD).
 *
 * Non-blocking: connection happens in the background. Call
 * wifi_manager_get_info() to poll the status.
 *
 * @return ESP_OK on success (init done, connect started).
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Reconnect to the configured AP. No-op if already connected
 *        or still connecting. Use this after a failed connection to
 *        trigger a fresh attempt.
 * @return ESP_OK if a connect attempt was started.
 */
esp_err_t wifi_manager_reconnect(void);

/**
 * @brief Disconnect from the AP and stop auto-reconnect.
 * @return ESP_OK on success.
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Get the current WiFi status snapshot.
 * @param[out] info  Filled with the current state. May be NULL to
 *                   just check wifi_manager_is_connected().
 */
void wifi_manager_get_info(wifi_info_t *info);

/**
 * @brief Check whether WiFi is connected and has an IP address.
 * @return true if connected.
 */
bool wifi_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
