#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_manager.h"

static const char *TAG = "mqtt_manager";

/* How often we poll the WiFi state to (re)start the MQTT client. */
#define WIFI_CHECK_PERIOD_US (2 * 1000 * 1000)

/* Last-will payload sent by the broker if the device drops unexpectedly. */
#define LWT_OFFLINE_MSG "{\"state\":\"offline\"}"

#define NVS_NS           "mqtt"
#define NVS_KEY_CFG      "cfg"
#define NVS_KEY_INTERVAL "interval"

/* Default keepalive when the config keeps 0 */
#define MQTT_DEFAULT_KEEPALIVE_S 120

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_mqtt_connected = false;
static bool s_wifi_was_connected = false;
static esp_timer_handle_t s_wifi_timer = NULL;

static char s_device_id[24];
static char s_telemetry_topic[64];
static char s_status_topic[64];
static char s_cmd_topic[64];
static char s_cmd_resp_topic[64];

/* Runtime configuration (loaded from NVS, falls back to Kconfig) */
static mqtt_cfg_t s_cfg = {0};

/* Telemetry rate limiting */
static int64_t s_last_publish_us = 0;
static int64_t s_interval_us = CONFIG_MQTT_TELEMETRY_INTERVAL * 1000000LL;
static volatile uint32_t s_publish_count = 0;

/* Recent command history (ring buffer) */
static mqtt_cmd_entry_t s_cmd_hist[MQTT_CMD_HISTORY_MAX];
static size_t s_cmd_hist_count = 0;
static size_t s_cmd_hist_pos = 0; /* next write slot */

/* Extra topic subscriptions registered via mqtt_manager_subscribe().
 * The command topic itself is handled separately (see handle_cmd). */
#define MQTT_EXT_SUB_MAX 4

typedef struct {
    char topic[64];
    int qos;
    mqtt_topic_handler_t handler;
} mqtt_sub_entry_t;

static mqtt_sub_entry_t s_ext_subs[MQTT_EXT_SUB_MAX];

/* ============================ NVS config ============================ */

/* Apply Kconfig defaults, then let NVS override them. */
static void cfg_load(void)
{
    snprintf(s_cfg.uri, sizeof(s_cfg.uri), "%s", CONFIG_MQTT_BROKER_URI);
    strlcpy(s_cfg.username, CONFIG_MQTT_USERNAME, sizeof(s_cfg.username));
    strlcpy(s_cfg.password, CONFIG_MQTT_PASSWORD, sizeof(s_cfg.password));
    s_cfg.client_id[0] = '\0';
    s_cfg.keepalive_s = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_cfg);
    nvs_get_blob(h, NVS_KEY_CFG, &s_cfg, &len);

    int32_t interval = 0;
    if (nvs_get_i32(h, NVS_KEY_INTERVAL, &interval) == ESP_OK &&
        interval >= 1 && interval <= 3600) {
        s_interval_us = interval * 1000000LL;
    }
    nvs_close(h);
}

static esp_err_t cfg_store(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(h, NVS_KEY_CFG, &s_cfg, sizeof(s_cfg));
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

/* ============================ helpers ============================ */

static void cmd_history_add(const char *text, int len)
{
    char *dst = s_cmd_hist[s_cmd_hist_pos].text;
    size_t cap = sizeof(s_cmd_hist[s_cmd_hist_pos].text);
    size_t n = len < (int)cap - 1 ? (size_t)len : cap - 1;
    memcpy(dst, text, n);
    dst[n] = '\0';

    s_cmd_hist_pos = (s_cmd_hist_pos + 1) % MQTT_CMD_HISTORY_MAX;
    if (s_cmd_hist_count < MQTT_CMD_HISTORY_MAX) {
        s_cmd_hist_count++;
    }
}

/* Publish a (retained) status message so subscribers see online/offline. */
static void publish_status(const char *state)
{
    if (s_client == NULL) {
        return;
    }
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"id\":\"%s\"}", state, s_device_id);
    esp_mqtt_client_publish(s_client, s_status_topic, payload, 0, 1, 1);
}

/* Handle a command received on devices/{id}/cmd.
 * Supported: {"cmd":"ping"}, {"cmd":"reboot"},
 *            {"cmd":"set_interval","value":<1..3600>} */
static void handle_cmd(const char *payload, int payload_len)
{
    cmd_history_add(payload, payload_len);

    char buf[128];
    int len = payload_len < (int)sizeof(buf) - 1 ? payload_len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = '\0';
    ESP_LOGI(TAG, "Command received: %s", buf);

    if (strstr(buf, "\"reboot\"")) {
        esp_mqtt_client_publish(s_client, s_cmd_resp_topic,
                                "{\"cmd\":\"reboot\",\"result\":\"ok\"}", 0, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the ack leave */
        esp_restart();
    } else if (strstr(buf, "\"set_interval\"")) {
        char *vp = strstr(buf, "\"value\"");
        if (vp != NULL) {
            vp = strchr(vp, ':');
            if (vp != NULL) {
                long sec = strtol(vp + 1, NULL, 10);
                mqtt_manager_set_interval((int)sec);
            }
        }
        esp_mqtt_client_publish(s_client, s_cmd_resp_topic,
                                "{\"cmd\":\"set_interval\",\"result\":\"ok\"}", 0, 1, 0);
    } else if (strstr(buf, "\"ping\"")) {
        esp_mqtt_client_publish(s_client, s_cmd_resp_topic,
                                "{\"cmd\":\"ping\",\"result\":\"pong\"}", 0, 1, 0);
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", buf);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "Connected to broker");
        publish_status("online");
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        /* Extra registered subscriptions (re)subscribe on every connect */
        for (size_t i = 0; i < MQTT_EXT_SUB_MAX; i++) {
            if (s_ext_subs[i].handler != NULL && s_ext_subs[i].topic[0] != '\0') {
                esp_mqtt_client_subscribe(s_client, s_ext_subs[i].topic,
                                          s_ext_subs[i].qos);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "Disconnected from broker");
        break;

    case MQTT_EVENT_DATA:
        if (event->topic != NULL &&
            strncmp(event->topic, s_cmd_topic, event->topic_len) == 0) {
            handle_cmd(event->data, event->data_len);
        }
        /* Dispatch to extra registered subscriptions (exact topic match) */
        for (size_t i = 0; i < MQTT_EXT_SUB_MAX; i++) {
            if (s_ext_subs[i].handler == NULL || s_ext_subs[i].topic[0] == '\0') {
                continue;
            }
            size_t tlen = strlen(s_ext_subs[i].topic);
            if (event->topic != NULL && event->topic_len == (int)tlen &&
                memcmp(event->topic, s_ext_subs[i].topic, tlen) == 0) {
                s_ext_subs[i].handler(event->topic, event->topic_len,
                                      event->data, event->data_len);
            }
        }
        break;

    case MQTT_EVENT_ERROR: {
        esp_mqtt_error_codes_t *err = event->error_handle;
        if (err != NULL) {
            ESP_LOGW(TAG, "MQTT error: tls=%s esp=%s",
                     esp_err_to_name(err->esp_tls_last_esp_err),
                     esp_err_to_name(err->esp_tls_stack_err));
        }
        break;
    }

    default:
        break;
    }
}

/* Create and start the MQTT client (once). Reconnects are handled
 * internally by esp-mqtt after this. */
static void mqtt_start(void)
{
    if (s_client != NULL) {
        return;
    }
    ESP_LOGI(TAG, "Starting MQTT client: %s (user: %s)",
             s_cfg.uri, s_cfg.username[0] ? s_cfg.username : "anonymous");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg.uri,
        /* Verify the server against the ESP-IDF certificate bundle
         * (trusts public CAs such as Let's Encrypt). */
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username =
            s_cfg.username[0] ? s_cfg.username : NULL,
        .credentials.authentication.password =
            s_cfg.password[0] ? s_cfg.password : NULL,
        .credentials.client_id =
            s_cfg.client_id[0] ? s_cfg.client_id : NULL,
        .session.keepalive =
            s_cfg.keepalive_s > 0 ? s_cfg.keepalive_s : MQTT_DEFAULT_KEEPALIVE_S,
        .session.last_will.topic = s_status_topic,
        .session.last_will.msg = LWT_OFFLINE_MSG,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
}

/* Tear the client down so a new config can be applied. */
static void mqtt_teardown(void)
{
    if (s_client != NULL) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_mqtt_connected = false;
}

/* Poll the WiFi state; on the disconnected->connected edge, start MQTT.
 * WiFi drops after that are handled by esp-mqtt's own reconnect. */
static void wifi_poll_cb(void *arg)
{
    (void)arg;
    bool connected = wifi_manager_is_connected();
    if (connected && !s_wifi_was_connected) {
        ESP_LOGI(TAG, "WiFi up, starting MQTT");
        mqtt_start();
    }
    s_wifi_was_connected = connected;
}

/* ============================ public API ============================ */

esp_err_t mqtt_manager_init(void)
{
    /* Load runtime config (NVS over Kconfig defaults) */
    cfg_load();

    /* Device id: esp32s3_<last 3 MAC bytes> (unique per device) */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp32s3_%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    snprintf(s_telemetry_topic, sizeof(s_telemetry_topic),
             "devices/%s/telemetry", s_device_id);
    snprintf(s_status_topic, sizeof(s_status_topic),
             "devices/%s/status", s_device_id);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic),
             "devices/%s/cmd", s_device_id);
    snprintf(s_cmd_resp_topic, sizeof(s_cmd_resp_topic),
             "devices/%s/cmd_resp", s_device_id);

    ESP_LOGI(TAG, "Device id: %s", s_device_id);
    ESP_LOGI(TAG, "Broker: %s", s_cfg.uri);
    ESP_LOGI(TAG, "Telemetry topic: %s", s_telemetry_topic);

    /* Watch the WiFi state; MQTT starts once WiFi is up */
    esp_timer_create_args_t args = {
        .callback = wifi_poll_cb,
        .name = "mqtt_wifi_poll",
    };
    esp_err_t ret = esp_timer_create(&args, &s_wifi_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi poll timer create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    return esp_timer_start_periodic(s_wifi_timer, WIFI_CHECK_PERIOD_US);
}

bool mqtt_manager_is_connected(void)
{
    return s_mqtt_connected;
}

void mqtt_manager_publish_telemetry(float temp, float humi)
{
    if (s_client == NULL || !s_mqtt_connected) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_publish_us < s_interval_us) {
        return; /* rate-limited */
    }
    s_last_publish_us = now;

    /* Snapshot WiFi info for rssi / ip / reconnect count */
    wifi_info_t winfo;
    wifi_manager_get_info(&winfo);

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"temp\":%.1f,\"humi\":%.1f,\"rssi\":%d,\"ip\":\"%s\",\"reconn\":%lu}",
             temp, humi, (int)winfo.rssi, winfo.ip,
             (unsigned long)winfo.reconnect_cnt);
    if (esp_mqtt_client_publish(s_client, s_telemetry_topic, payload, 0, 0, 0) >= 0) {
        s_publish_count++;
    }
}

const char *mqtt_manager_get_device_id(void)
{
    return s_device_id;
}

const char *mqtt_manager_get_telemetry_topic(void)
{
    return s_telemetry_topic;
}

const char *mqtt_manager_get_status_topic(void)
{
    return s_status_topic;
}

const char *mqtt_manager_get_cmd_topic(void)
{
    return s_cmd_topic;
}

const char *mqtt_manager_get_cmd_resp_topic(void)
{
    return s_cmd_resp_topic;
}

uint32_t mqtt_manager_get_publish_count(void)
{
    return s_publish_count;
}

void mqtt_manager_get_cfg(mqtt_cfg_t *out)
{
    if (out != NULL) {
        *out = s_cfg;
    }
}

esp_err_t mqtt_manager_set_cfg(const mqtt_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    /* Keep sensible bounds */
    s_cfg.uri[sizeof(s_cfg.uri) - 1] = '\0';
    s_cfg.username[sizeof(s_cfg.username) - 1] = '\0';
    s_cfg.password[sizeof(s_cfg.password) - 1] = '\0';
    s_cfg.client_id[sizeof(s_cfg.client_id) - 1] = '\0';

    esp_err_t ret = cfg_store();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Restart the client with the new config */
    mqtt_teardown();
    if (wifi_manager_is_connected()) {
        mqtt_start();
    }
    return ESP_OK;
}

int mqtt_manager_get_interval(void)
{
    return (int)(s_interval_us / 1000000LL);
}

esp_err_t mqtt_manager_set_interval(int seconds)
{
    if (seconds < 1) {
        seconds = 1;
    }
    if (seconds > 3600) {
        seconds = 3600;
    }
    s_interval_us = seconds * 1000000LL;
    ESP_LOGI(TAG, "Telemetry interval -> %d s", seconds);

    /* Persist for the next boot */
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret == ESP_OK) {
        ret = nvs_set_i32(h, NVS_KEY_INTERVAL, seconds);
        if (ret == ESP_OK) {
            ret = nvs_commit(h);
        }
        nvs_close(h);
    }
    return ret;
}

esp_err_t mqtt_manager_disconnect(void)
{
    if (s_client == NULL) {
        return ESP_OK;
    }
    esp_mqtt_client_stop(s_client);
    s_mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT disconnected by user");
    return ESP_OK;
}

esp_err_t mqtt_manager_connect(void)
{
    if (s_mqtt_connected) {
        return ESP_OK; /* already connected */
    }
    if (s_client == NULL) {
        if (wifi_manager_is_connected()) {
            mqtt_start();
            return ESP_OK;
        }
        ESP_LOGW(TAG, "WiFi not connected, cannot start MQTT");
        return ESP_FAIL;
    }
    esp_mqtt_client_start(s_client);
    return ESP_OK;
}

size_t mqtt_manager_get_cmd_history(mqtt_cmd_entry_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    size_t n = s_cmd_hist_count < max ? s_cmd_hist_count : max;
    size_t start = (s_cmd_hist_pos + MQTT_CMD_HISTORY_MAX - s_cmd_hist_count) %
                   MQTT_CMD_HISTORY_MAX;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_cmd_hist[(start + i) % MQTT_CMD_HISTORY_MAX];
    }
    return n;
}

esp_err_t mqtt_manager_subscribe(const char *topic, int qos,
                                 mqtt_topic_handler_t handler)
{
    if (topic == NULL || topic[0] == '\0' || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (qos < 0) {
        qos = 0;
    }
    if (qos > 2) {
        qos = 2;
    }

    /* Re-registering the same topic replaces the old entry. */
    for (size_t i = 0; i < MQTT_EXT_SUB_MAX; i++) {
        if (s_ext_subs[i].handler != NULL &&
            strcmp(s_ext_subs[i].topic, topic) == 0) {
            strlcpy(s_ext_subs[i].topic, topic, sizeof(s_ext_subs[i].topic));
            s_ext_subs[i].qos = qos;
            s_ext_subs[i].handler = handler;
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < MQTT_EXT_SUB_MAX; i++) {
        if (s_ext_subs[i].handler == NULL) {
            strlcpy(s_ext_subs[i].topic, topic, sizeof(s_ext_subs[i].topic));
            s_ext_subs[i].qos = qos;
            s_ext_subs[i].handler = handler;
            ESP_LOGI(TAG, "Extra subscription registered: %s (qos %d)",
                     topic, qos);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "Extra subscription table full (%d)", MQTT_EXT_SUB_MAX);
    return ESP_ERR_NO_MEM;
}
