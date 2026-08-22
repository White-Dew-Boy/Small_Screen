#include "wifi_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_manager";

/* Kconfig credentials (see main/Kconfig.projbuild) */
#define WIFI_SSID     CONFIG_WIFI_SSID
#define WIFI_PASSWORD CONFIG_WIFI_PASSWORD
#define WIFI_MAX_RETRY CONFIG_WIFI_MAX_RETRY

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
};
static int s_retry_cnt = 0;
static bool s_auto_reconnect = true;

static void set_state(wifi_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.state = st;
    xSemaphoreGive(s_lock);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting to \"%s\"", WIFI_SSID);
        set_state(WIFI_STATE_CONNECTING);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP, reason: %d", disc->reason);

        if (!s_auto_reconnect) {
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
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_cnt = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.state = WIFI_STATE_CONNECTED;
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

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* Adjust for faster (re)connect */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(WIFI_PASSWORD) == 0) {
        /* Open network */
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    /* Credentials come from Kconfig; keep them in RAM only, don't touch NVS */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");

    s_auto_reconnect = true;
    s_retry_cnt = 0;
    set_state(WIFI_STATE_DISCONNECTED);

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "WiFi manager initialized (SSID \"%s\")", WIFI_SSID);
    return ESP_OK;
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
