#include "time_manager.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

static const char *TAG = "time_manager";

/* True once SNTP has been started. lwIP SNTP keeps retrying on its own
 * (including after reconnects), so it only needs to be started once. */
static bool s_started = false;

/* True once at least one SNTP sync has completed (set by the sync
 * callback). NOTE: do NOT poll esp_sntp_get_sync_status() for this — that
 * API consumes (resets) the status on every read, so a 1 s poll would only
 * see COMPLETED once and then report RESET forever. */
static bool s_synced = false;

/* Called by lwIP SNTP after every successful time sync (first sync and
 * each periodic re-sync). Runs in the lwIP context; only sets a flag. */
static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "time synchronized");
}

/* Start SNTP on the first STA_GOT_IP. The timezone was already applied by
 * time_manager_init(), so esp_sntp_init() stores UTC and localtime()
 * converts with the configured TZ. */
static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;

    if (s_started) {
        return;
    }
    s_started = true;

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_TIME_NTP_SERVER);
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    /* Second server for redundancy (esp_sntp tries servers in order) */
    esp_sntp_setservername(1, "ntp.aliyun.com");
#endif
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (server: %s, TZ: %s)",
             CONFIG_TIME_NTP_SERVER, CONFIG_TIME_TZ);
}

esp_err_t time_manager_init(void)
{
    /* POSIX timezone for localtime(); must be set before the first
     * localtime() call. "CST-8" = China (UTC+8). */
    setenv("TZ", CONFIG_TIME_TZ, 1);
    tzset();

    /* Register the sync-completion flag BEFORE esp_sntp_init() runs
     * (it fires on the first sync at GOT_IP time). */
    esp_sntp_set_time_sync_notification_cb(on_time_sync);

    /* The default event loop is created by wifi_manager_init(). */
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            on_got_ip, NULL, NULL),
        TAG, "register IP_EVENT handler failed");

    ESP_LOGI(TAG, "time manager ready (TZ: %s)", CONFIG_TIME_TZ);
    return ESP_OK;
}

bool time_manager_is_synced(void)
{
    return s_synced;
}
