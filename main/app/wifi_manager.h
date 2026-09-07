#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Data Structures
 *============================================================================*/
/* Maximum number of APs kept from a scan (memory bound for the results cache) */
#define WIFI_SCAN_MAX_RESULTS 20

/* Maximum number of WiFi networks saved in NVS */
#define WIFI_MAX_SAVED_CREDS 5

/* One saved network credential */
typedef struct {
    char ssid[33]; /* Network name */
    char pass[65]; /* Password (empty = open network) */
} wifi_cred_t;

/* One scan result entry (snapshot copied by wifi_manager_scan_get_results()) */
typedef struct {
    char ssid[33];              /* Network name */
    int8_t rssi;                /* Signal strength in dBm */
    wifi_auth_mode_t authmode;  /* Security mode */
} wifi_scan_result_t;

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
    int16_t last_reason; /* Last disconnect reason (wifi_err_reason_t, 0 if none) */
} wifi_info_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize NVS, netif, event loop and WiFi in STA mode.
 *
 * WiFi credentials are loaded from NVS (saved on successful connections,
 * see wifi_manager_connect_new()). The first auto-connect candidate is
 * the last network that connected successfully; when it fails, the
 * manager falls back through the saved networks in order (each tried up
 * to CONFIG_WIFI_MAX_RETRY times) until one connects or all are
 * exhausted.
 *
 * Non-blocking. Call wifi_manager_get_info() to poll the status.
 *
 * @return ESP_OK on success (init done, connect started if credentials exist).
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Connect to a network with new credentials, saving them only on
 *        success.
 *
 * Keeps the credentials in RAM and starts a single connection attempt
 * without touching NVS. Once the link succeeds (GOT_IP) the network is
 * added to the saved list; on failure the attempt is discarded (no
 * retries) and nothing is stored, so a wrong password leaves no record
 * behind.
 *
 * @param[in] ssid     Network name (1..32 chars, must not be empty).
 * @param[in] password Password (up to 63 chars; empty string for open network).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad input.
 */
esp_err_t wifi_manager_connect_new(const char *ssid, const char *password);

/**
 * @brief Get the credentials currently stored in NVS.
 * @param[out] ssid        Buffer for the SSID (may be NULL).
 * @param[in]  ssid_cap    Capacity of the ssid buffer.
 * @param[out] password    Buffer for the password (may be NULL).
 * @param[in]  pass_cap    Capacity of the password buffer.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no credentials are saved.
 */
esp_err_t wifi_manager_get_credentials(char *ssid, size_t ssid_cap,
                                       char *password, size_t pass_cap);

/**
 * @brief Get the number of saved WiFi networks (0..WIFI_MAX_SAVED_CREDS).
 * @return Number of saved credentials.
 */
int wifi_manager_cred_count(void);

/**
 * @brief Get one saved credential by index.
 * @param[in] idx  Index into the saved list (0..cred_count()-1).
 * @return Pointer to the credential, or NULL if idx is out of range.
 */
const wifi_cred_t *wifi_manager_cred_get(int idx);

/**
 * @brief Connect using the idx-th saved credential.
 *
 * Loads the saved SSID/password as the active credentials and starts a
 * fresh connection attempt. On failure the device keeps retrying that
 * network (see CONFIG_WIFI_MAX_RETRY) and then falls back to the other
 * saved networks in order; the UI can show wifi_info.last_reason.
 *
 * @param[in] idx  Index into the saved list.
 * @return ESP_OK if the attempt was started, ESP_ERR_INVALID_ARG if idx
 *         is out of range.
 */
esp_err_t wifi_manager_connect_saved(int idx);

/**
 * @brief Forget (remove) the idx-th saved network.
 *
 * Deletes the entry from NVS so it is gone after a reboot. If the removed
 * network is the one the device is currently connected to or trying to
 * connect to, the active credentials are cleared and the link is dropped
 * with auto-reconnect disabled — the manager stops retrying a forgotten
 * (e.g. wrong-password) network.
 *
 * @param[in] idx  Index into the saved list (0..cred_count()-1).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if idx is out of range.
 */
esp_err_t wifi_manager_forget(int idx);

/**
 * @brief Check whether WiFi credentials are saved in NVS.
 * @return true if credentials exist.
 */
bool wifi_manager_has_credentials(void);

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
 * @brief Human-readable, short text for a WiFi disconnect reason code
 *        (wifi_info_t.last_reason, see esp_wifi_types_generic.h).
 *        Strings are kept short so they fit the 240 px display.
 * @param[in] reason  Disconnect reason code.
 * @return Static string (never NULL).
 */
const char *wifi_manager_reason_to_str(int16_t reason);

/**
 * @brief Check whether WiFi is connected and has an IP address.
 * @return true if connected.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Start an asynchronous scan for nearby 2.4 GHz access points.
 *
 * While scanning, an existing connection is dropped (a full-channel scan
 * requires the STA to be disconnected); if credentials are saved, the
 * manager reconnects automatically once the scan finishes.
 *
 * Non-blocking. Poll wifi_manager_scan_in_progress() until it returns
 * false, then call wifi_manager_scan_get_results().
 *
 * @return ESP_OK if the scan was started (or is already running).
 */
esp_err_t wifi_manager_scan_start(void);

/**
 * @brief Check whether a scan is currently in progress.
 * @return true while a scan is running.
 */
bool wifi_manager_scan_in_progress(void);

/**
 * @brief Copy the results of the last finished scan.
 * @param[out] results   Buffer receiving up to `capacity` entries.
 * @param[in]  capacity  Capacity of the buffer (use WIFI_SCAN_MAX_RESULTS).
 * @return Number of results copied.
 */
size_t wifi_manager_scan_get_results(wifi_scan_result_t *results, size_t capacity);

#ifdef __cplusplus
}
#endif
