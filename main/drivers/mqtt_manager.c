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

#include "wifi_manager.h"

static const char *TAG = "mqtt_manager";

/* How often we poll the WiFi state to (re)start the MQTT client. */
#define WIFI_CHECK_PERIOD_US (2 * 1000 * 1000)

/* Last-will payload sent by the broker if the device drops unexpectedly. */
#define LWT_OFFLINE_MSG "{\"state\":\"offline\"}"

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_mqtt_connected = false;
static bool s_wifi_was_connected = false;
static esp_timer_handle_t s_wifi_timer = NULL;

static char s_device_id[24];
static char s_telemetry_topic[64];
static char s_status_topic[64];
static char s_cmd_topic[64];
static char s_cmd_resp_topic[64];

static int64_t s_last_publish_us = 0;
static int64_t s_interval_us = CONFIG_MQTT_TELEMETRY_INTERVAL * 1000000LL;
static volatile uint32_t s_publish_count = 0;

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
                if (sec < 1) {
                    sec = 1;
                }
                if (sec > 3600) {
                    sec = 3600;
                }
                s_interval_us = sec * 1000000LL;
                ESP_LOGI(TAG, "Telemetry interval -> %ld s", sec);
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
             CONFIG_MQTT_BROKER_URI,
             CONFIG_MQTT_USERNAME[0] ? CONFIG_MQTT_USERNAME : "anonymous");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        /* Verify the server against the ESP-IDF certificate bundle
         * (trusts public CAs such as Let's Encrypt). */
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username =
            CONFIG_MQTT_USERNAME[0] ? CONFIG_MQTT_USERNAME : NULL,
        .credentials.authentication.password =
            CONFIG_MQTT_PASSWORD[0] ? CONFIG_MQTT_PASSWORD : NULL,
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

esp_err_t mqtt_manager_init(void)
{
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
    ESP_LOGI(TAG, "Telemetry topic: %s", s_telemetry_topic);
    ESP_LOGI(TAG, "Command topic: %s", s_cmd_topic);

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

uint32_t mqtt_manager_get_publish_count(void)
{
    return s_publish_count;
}
