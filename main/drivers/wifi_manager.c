#include "wifi_manager.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_manager";

/* Max retries from Kconfig (Component config -> WiFi Configuration) */
#define WIFI_MAX_RETRY CONFIG_WIFI_MAX_RETRY

#define NVS_NAMESPACE  "wifi"
#define NVS_KEY_CREDS  "creds"

/* Persisted credential list: count + up to WIFI_MAX_SAVED_CREDS entries. */
typedef struct {
    int32_t count;
    wifi_cred_t list[WIFI_MAX_SAVED_CREDS];
} creds_blob_t;

/* Shared status, guarded by s_lock.
 * The WiFi event handler runs in the esp_event task, while callers may
 * poll from any task — a mutex keeps the snapshot consistent. */
static SemaphoreHandle_t s_lock = NULL;
static wifi_info_t s_info = {
    .state = WIFI_STATE_IDLE,
    .ssid = {0},
    .ip = {0},
    .rssi = 0,
    .reconnect_cnt = 0,
    .last_reason = 0,
};
static int s_retry_cnt = 0;
static bool s_auto_reconnect = true;

/* Set while we initiate a disconnect ourselves (network switch, scan,
 * manual disconnect). The DISCONNECTED handler then treats the reason as
 * expected and keeps last_reason clear instead of surfacing a failure. */
static bool s_manual_switch = false;

/* Active credentials (the network we are currently connecting to). */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static bool s_has_creds = false;

/* Saved network list (persisted to NVS), guarded by s_lock. */
static creds_blob_t s_creds = {0};

/* Scan state: results cache (guarded by s_lock) + in-progress flag.
 * While a scan runs, auto-reconnect is suspended so the disconnect done
 * for the full-channel scan does not fight with the scan itself. */
static wifi_scan_result_t s_scan_results[WIFI_SCAN_MAX_RESULTS];
static size_t s_scan_count = 0;
static bool s_scan_in_progress = false;

static void set_state(wifi_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = st;
    xSemaphoreGive(s_lock);
}

/* Load the saved credential list from NVS. On success the first entry
 * (if any) becomes the active credentials for auto-connect. */
static esp_err_t creds_load(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = sizeof(s_creds);
    ret = nvs_get_blob(h, NVS_KEY_CREDS, &s_creds, &len);
    nvs_close(h);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_creds.count < 0) {
        s_creds.count = 0;
    }
    if (s_creds.count > WIFI_MAX_SAVED_CREDS) {
        s_creds.count = WIFI_MAX_SAVED_CREDS;
    }

    if (s_creds.count > 0) {
        strlcpy(s_ssid, s_creds.list[0].ssid, sizeof(s_ssid));
        strlcpy(s_pass, s_creds.list[0].pass, sizeof(s_pass));
        s_has_creds = true;
    }
    return ESP_OK;
}

/* Persist the credential list to NVS. */
static esp_err_t creds_store(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(h, NVS_KEY_CREDS, &s_creds, sizeof(s_creds));
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

/* Add or update a credential in the saved list (max WIFI_MAX_SAVED_CREDS,
 * oldest entry is overwritten when full). Persists to NVS. */
static esp_err_t creds_update(const char *ssid, const char *pass)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Update in place if already saved */
    for (int i = 0; i < s_creds.count; i++) {
        if (strcmp(s_creds.list[i].ssid, ssid) == 0) {
            strlcpy(s_creds.list[i].pass, pass, sizeof(s_creds.list[i].pass));
            goto store;
        }
    }

    /* New network: append, or overwrite the oldest when full */
    if (s_creds.count < WIFI_MAX_SAVED_CREDS) {
        strlcpy(s_creds.list[s_creds.count].ssid, ssid,
                sizeof(s_creds.list[s_creds.count].ssid));
        strlcpy(s_creds.list[s_creds.count].pass, pass,
                sizeof(s_creds.list[s_creds.count].pass));
        s_creds.count++;
    } else {
        /* Shift left, drop the oldest */
        for (int i = 1; i < WIFI_MAX_SAVED_CREDS; i++) {
            s_creds.list[i - 1] = s_creds.list[i];
        }
        strlcpy(s_creds.list[WIFI_MAX_SAVED_CREDS - 1].ssid, ssid,
                sizeof(s_creds.list[WIFI_MAX_SAVED_CREDS - 1].ssid));
        strlcpy(s_creds.list[WIFI_MAX_SAVED_CREDS - 1].pass, pass,
                sizeof(s_creds.list[WIFI_MAX_SAVED_CREDS - 1].pass));
    }

store:
    xSemaphoreGive(s_lock);
    return creds_store();
}

/* Apply the in-RAM credentials to the WiFi driver config. */
static esp_err_t creds_apply_to_wifi(void)
{
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)wifi_cfg.sta.ssid, s_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, s_pass, sizeof(wifi_cfg.sta.password));
    if (strlen(s_pass) == 0) {
        /* Open network */
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
        if (s_has_creds) {
            ESP_LOGI(TAG, "Connecting to \"%s\"", s_ssid);
            set_state(WIFI_STATE_CONNECTING);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "No credentials saved — configure WiFi from the UI");
            set_state(WIFI_STATE_DISCONNECTED);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %d", disc->reason);

        /* Remember the reason so the UI can show why (201: AP not found,
         * 202: wrong password, 204: handshake timeout, ...). An initiated
         * disconnect (network switch / scan / manual) is not a failure:
         * keep last_reason clear so the UI does not report it as one. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_manual_switch) {
            s_info.last_reason = 0;
        } else {
            s_info.last_reason = disc->reason;
        }
        s_manual_switch = false;
        xSemaphoreGive(s_lock);

        /* While a scan is running, do not auto-reconnect: the scan itself
         * disconnected the STA, and reconnecting mid-scan would abort the
         * scan or restrict it to the current channel. wifi_manager_scan_start()
         * / the SCAN_DONE handler resumes the connection afterwards. */
        if (!s_auto_reconnect || s_scan_in_progress) {
            set_state(WIFI_STATE_DISCONNECTED);
            return;
        }

        if (s_retry_cnt < WIFI_MAX_RETRY) {
            s_retry_cnt++;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_info.reconnect_cnt++;
            xSemaphoreGive(s_lock);
            set_state(WIFI_STATE_CONNECTING);
            ESP_LOGI(TAG, "Reconnect attempt %d/%d", s_retry_cnt, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            set_state(WIFI_STATE_DISCONNECTED);
            ESP_LOGW(TAG, "Giving up after %d retries", s_retry_cnt);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        /* Copy the results out of the wifi lib before they are freed. */
        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);
        ESP_LOGI(TAG, "Scan finished, %u AP(s) found", (unsigned)ap_num);

        uint16_t to_copy = ap_num < WIFI_SCAN_MAX_RESULTS ? ap_num : WIFI_SCAN_MAX_RESULTS;
        wifi_ap_record_t *aps = malloc(to_copy * sizeof(wifi_ap_record_t));
        if (aps != NULL) {
            uint16_t copied = to_copy;
            if (esp_wifi_scan_get_ap_records(&copied, aps) == ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_scan_count = copied;
                for (uint16_t i = 0; i < copied; i++) {
                    strlcpy(s_scan_results[i].ssid, (char *)aps[i].ssid,
                            sizeof(s_scan_results[i].ssid));
                    s_scan_results[i].rssi = aps[i].rssi;
                    s_scan_results[i].authmode = aps[i].authmode;
                }
                xSemaphoreGive(s_lock);
            }
            free(aps);
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_scan_count = 0;
            xSemaphoreGive(s_lock);
        }

        s_scan_in_progress = false;

        /* Resume the connection that the scan dropped, if any */
        if (s_auto_reconnect && s_has_creds) {
            wifi_info_t info;
            wifi_manager_get_info(&info);
            if (info.state != WIFI_STATE_CONNECTED &&
                info.state != WIFI_STATE_CONNECTING) {
                set_state(WIFI_STATE_CONNECTING);
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_cnt = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.state = WIFI_STATE_CONNECTED;
        s_info.last_reason = 0; /* clear any previous failure */
        snprintf(s_info.ip, sizeof(s_info.ip), IPSTR, IP2STR(&event->ip_info.ip));
        /* SSID is refreshed lazily in wifi_manager_get_info() via
         * esp_wifi_sta_get_ap_info(). */
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t nvs_flash_init_safe(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init_safe(), TAG, "NVS init failed");

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "lock create failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta_netif != NULL, ESP_FAIL, TAG, "wifi sta netif create failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &event_handler, NULL, NULL),
        TAG, "wifi event register failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &event_handler, NULL, NULL),
        TAG, "ip event register failed");

    /* Load saved credentials from NVS (if any) */
    esp_err_t cred_ret = creds_load();
    if (cred_ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded credentials for \"%s\"", s_ssid);
    } else {
        ESP_LOGW(TAG, "No saved credentials (%s) — configure WiFi from the UI",
                 esp_err_to_name(cred_ret));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    /* Credentials live in our own NVS storage; keep the wifi lib config in RAM */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    if (cred_ret == ESP_OK) {
        ESP_RETURN_ON_ERROR(creds_apply_to_wifi(), TAG, "set config failed");
    }

    s_auto_reconnect = true;
    s_retry_cnt = 0;
    set_state(WIFI_STATE_DISCONNECTED);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "WiFi manager initialized (credentials: %s)",
             s_has_creds ? "yes" : "none");
    return ESP_OK;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0 || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "Invalid SSID");
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL || strlen(password) > 63) {
        ESP_LOGE(TAG, "Invalid password");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Saving credentials for \"%s\"", ssid);
    ESP_RETURN_ON_ERROR(creds_update(ssid, password), TAG, "NVS save failed");
    ESP_RETURN_ON_ERROR(creds_apply_to_wifi(), TAG, "apply credentials failed");

    /* Mirror the new credentials as active */
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    s_has_creds = true;

    s_auto_reconnect = true;
    s_retry_cnt = 0;

    wifi_info_t info;
    wifi_manager_get_info(&info);

    /* Already connected to this exact network: keep the link. Calling
     * esp_wifi_connect() on a connected STA fails (ESP_ERR_WIFI_CONN) and
     * the CONNECTING state would never be resolved by an event. */
    if (info.state == WIFI_STATE_CONNECTED && strcmp(info.ssid, ssid) == 0) {
        ESP_LOGI(TAG, "Already connected to \"%s\", keeping connection", ssid);
        return ESP_OK;
    }

    /* Connected to a different network: drop the link; the DISCONNECTED
     * handler reconnects immediately with the new credentials. */
    if (info.state == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Switching from \"%s\" to \"%s\"", info.ssid, ssid);
        set_state(WIFI_STATE_CONNECTING);
        s_manual_switch = true;
        esp_wifi_disconnect();
        return ESP_OK;
    }

    /* Not connected: start a fresh connection attempt. */
    set_state(WIFI_STATE_CONNECTING);
    s_manual_switch = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.last_reason = 0; /* clear any previous failure */
    xSemaphoreGive(s_lock);
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s (will connect on STA start)",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

int wifi_manager_cred_count(void)
{
    int count = 0;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    count = s_creds.count;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return count;
}

const wifi_cred_t *wifi_manager_cred_get(int idx)
{
    const wifi_cred_t *cred = NULL;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (idx >= 0 && idx < s_creds.count) {
        cred = &s_creds.list[idx];
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return cred;
}

esp_err_t wifi_manager_connect_saved(int idx)
{
    const wifi_cred_t *cred = wifi_manager_cred_get(idx);
    if (cred == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to saved network \"%s\"", cred->ssid);
    strlcpy(s_ssid, cred->ssid, sizeof(s_ssid));
    strlcpy(s_pass, cred->pass, sizeof(s_pass));
    s_has_creds = true;

    ESP_RETURN_ON_ERROR(creds_apply_to_wifi(), TAG, "apply credentials failed");

    s_auto_reconnect = true;
    s_retry_cnt = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.last_reason = 0; /* clear any previous failure */
    xSemaphoreGive(s_lock);

    wifi_info_t info;
    wifi_manager_get_info(&info);

    /* Already connected to this exact network: keep the link. Calling
     * esp_wifi_connect() on a connected STA fails and the CONNECTING
     * state would never be resolved. */
    if (info.state == WIFI_STATE_CONNECTED &&
        strcmp(info.ssid, cred->ssid) == 0) {
        ESP_LOGI(TAG, "Already connected to \"%s\", keeping connection",
                 cred->ssid);
        return ESP_OK;
    }

    /* Connected to a different network: drop the link; the DISCONNECTED
     * handler reconnects immediately with the new credentials. */
    if (info.state == WIFI_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Switching from \"%s\" to \"%s\"", info.ssid,
                 cred->ssid);
        set_state(WIFI_STATE_CONNECTING);
        s_manual_switch = true;
        esp_wifi_disconnect();
        return ESP_OK;
    }

    /* Not connected: start a fresh connection attempt. */
    set_state(WIFI_STATE_CONNECTING);
    s_manual_switch = false;
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s (will connect on STA start)",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
}

esp_err_t wifi_manager_get_credentials(char *ssid, size_t ssid_cap,
                                       char *password, size_t pass_cap)
{
    if (!s_has_creds) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ssid != NULL && ssid_cap > 0) {
        strlcpy(ssid, s_ssid, ssid_cap);
    }
    if (password != NULL && pass_cap > 0) {
        strlcpy(password, s_pass, pass_cap);
    }
    return ESP_OK;
}

bool wifi_manager_has_credentials(void)
{
    return s_has_creds;
}

esp_err_t wifi_manager_reconnect(void)
{
    wifi_info_t info;
    wifi_manager_get_info(&info);
    if (info.state == WIFI_STATE_CONNECTED || info.state == WIFI_STATE_CONNECTING) {
        return ESP_OK;
    }

    s_auto_reconnect = true;
    s_retry_cnt = 0;
    set_state(WIFI_STATE_CONNECTING);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect(void)
{
    s_auto_reconnect = false;
    set_state(WIFI_STATE_DISCONNECTED);
    s_manual_switch = true; /* initiated by us, not a failure */
    return esp_wifi_disconnect();
}

void wifi_manager_get_info(wifi_info_t *info)
{
    if (info == NULL) {
        return;
    }

    /* s_lock is created in wifi_manager_init(); before that only the
     * initial static snapshot is available. */
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        *info = s_info;
        xSemaphoreGive(s_lock);
    } else {
        *info = s_info;
    }

    /* Refresh RSSI on demand while connected (thread-safe inside wifi lib) */
    if (info->state == WIFI_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            info->rssi = ap_info.rssi;
            memcpy(info->ssid, (char *)ap_info.ssid, sizeof(info->ssid));
            info->ssid[sizeof(info->ssid) - 1] = '\0';
        }
    }
}

bool wifi_manager_is_connected(void)
{
    wifi_info_t info;
    wifi_manager_get_info(&info);
    return info.state == WIFI_STATE_CONNECTED;
}

esp_err_t wifi_manager_scan_start(void)
{
    if (s_scan_in_progress) {
        return ESP_OK; /* already scanning */
    }

    /* A full-channel scan needs the STA disconnected; otherwise esp_wifi
     * only scans the current channel. Drop the link now — auto-reconnect is
     * suspended during the scan and resumed in the SCAN_DONE handler. */
    wifi_info_t info;
    wifi_manager_get_info(&info);
    if (info.state == WIFI_STATE_CONNECTED || info.state == WIFI_STATE_CONNECTING) {
        s_manual_switch = true; /* scan-initiated disconnect is expected */
        esp_wifi_disconnect();
    }

    s_scan_count = 0;
    s_scan_in_progress = true;

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,            /* all 2.4 GHz channels */
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100, /* ms per channel */
        .scan_time.active.max = 300,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false); /* async */
    if (ret != ESP_OK) {
        s_scan_in_progress = false;
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

bool wifi_manager_scan_in_progress(void)
{
    return s_scan_in_progress;
}

size_t wifi_manager_scan_get_results(wifi_scan_result_t *results, size_t capacity)
{
    if (results == NULL || capacity == 0) {
        return 0;
    }

    size_t n = 0;
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    n = s_scan_count < capacity ? s_scan_count : capacity;
    if (n > 0) {
        memcpy(results, s_scan_results, n * sizeof(wifi_scan_result_t));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return n;
}
