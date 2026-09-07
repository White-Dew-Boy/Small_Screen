/* PC performance data over MQTT (alternate transport to app/ble_perf.c).
 *
 * The PC publishes a small flat JSON object to the configured topic
 * (default "pc/performance", see CONFIG_MQTT_PC_PERF_TOPIC) at QoS 1:
 *   {"timestamp":1788590904.21,"cpu":3.1,"memory":41.3,
 *    "upload_speed":1.658,"download_speed":26.877,"gpu":0.0,"disk":24.3}
 *   cpu/memory/gpu/disk in %, upload_speed/download_speed in KB/s.
 *   "timestamp" is wall-clock seconds and informational only: like the BLE
 *   driver, we stamp the reception time ourselves.
 *
 * Data validity (see materials/mqtt_payload.md): a KEY PRESENT in the JSON
 * means the field has data — 0 is a legal measured value (GPU idle 0%).
 * A MISSING key means the field is not collected (e.g. GPU collection off)
 * and must be shown as "--", never as 0. This is the JSON equivalent of
 * the BLE v2 flags bitmap.
 *
 * Parsing: the payload is a single-level, numeric-only JSON object, so a
 * tiny purpose-built key reader is used instead of a full JSON library
 * (ESP-IDF 6 no longer ships a cJSON component, and this board is short on
 * internal RAM — every KB of flash/RAM counts).
 *
 * The parsed values are published into the SAME pc_perf_data_t snapshot
 * shape as the BLE driver (see app/ble_perf.h), so the UI page shows
 * data from whichever transport is live. temp_c/fps keys are never sent
 * by the PC -> their presence bits never get set (rows auto-hide).
 *
 * Tasking: the esp-mqtt task calls pc_perf_mqtt_msg() and writes the
 * snapshot; the LVGL thread reads it via pc_perf_mqtt_get_data().
 */

#include "pc_perf_mqtt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "mqtt_manager.h"

static const char *TAG = "pc_perf_mqtt";

/* The PC frame is ~150 bytes; 512 is generous headroom. */
#define MQTT_PERF_MAX_PAYLOAD 512

/* Kconfig symbol from main/Kconfig.projbuild (default "pc/performance").
 * Fall back in case the build was never reconfigured. */
#ifndef CONFIG_MQTT_PC_PERF_TOPIC
#define CONFIG_MQTT_PC_PERF_TOPIC "pc/performance"
#endif

/* Shared snapshot (written by the esp-mqtt task, read by LVGL) */
static pc_perf_data_t s_data;

static bool s_init_done = false;
static bool s_ever = false;   /* at least one valid frame received */
static uint32_t s_frames = 0; /* total frames received (logging only) */

/* NUL-terminated scratch copy of the current payload (static: the esp-mqtt
 * data is not NUL-terminated and we must not stack-allocate in a task) */
static char s_buf[MQTT_PERF_MAX_PAYLOAD];

/*---------------------------------------------------------------------------
 * Minimal flat-JSON number reader
 *---------------------------------------------------------------------------*/

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Find '"key": <number>' inside a flat JSON object. Returns true and sets
 * *out when found. The quoted-key search cannot collide with longer keys
 * (e.g. "cpu" vs "cpu_load"), because the closing quote is part of the
 * needle. A numeric value must end at a JSON delimiter, so malformed
 * numbers like 3.1.5 are rejected. */
static bool json_number(const char *json, const char *key, float *out)
{
    char needle[48];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(needle) || out == NULL) {
        return false;
    }

    const char *p = json;
    while (p != NULL && (p = strstr(p, needle)) != NULL) {
        const char *q = p + n; /* just after the closing quote */
        while (is_ws(*q)) {
            q++;
        }
        if (*q != ':') {
            p += 1;
            continue;
        }
        q++;
        while (is_ws(*q)) {
            q++;
        }
        if (*q == '-' || (*q >= '0' && *q <= '9')) {
            char *end = NULL;
            double v = strtod(q, &end);
            if (end != q &&
                (*end == '\0' || *end == ',' || *end == '}' || *end == ']' ||
                 is_ws(*end))) {
                *out = (float)v;
                return true;
            }
        }
        p += 1; /* keep scanning for a later occurrence */
    }
    return false;
}

/*---------------------------------------------------------------------------
 * Message handler (esp-mqtt task context)
 *---------------------------------------------------------------------------*/

static void pc_perf_mqtt_msg(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    (void)topic;
    (void)topic_len;

    if (data == NULL || data_len <= 0) {
        return;
    }
    int len = data_len;
    if (len >= (int)sizeof(s_buf)) {
        len = (int)sizeof(s_buf) - 1;
    }
    memcpy(s_buf, data, len);
    s_buf[len] = '\0';

    /* Per-key: presence bit + snapshot member. Data validity follows the
     * mqtt_payload.md rule — a KEY PRESENT in the JSON means the field has
     * data (0 is a legal measured value, e.g. GPU idle 0%), a MISSING key
     * means "not collected" (GPU collection off / no GPU) and the row must
     * be hidden. This mirrors the BLE v2 flags semantics. Values of keys
     * absent from the current message are kept but NOT flagged as present,
     * so the UI never shows them. temp/fps keys are never sent -> their
     * presence bits never get set. */
    static const struct {
        const char *key;
        uint8_t bit;
        volatile float *dst;
    } fields[] = {
        { "cpu",            PC_PERF_P_CPU,  &s_data.cpu_pct },
        { "memory",         PC_PERF_P_MEM,  &s_data.mem_pct },
        { "upload_speed",   PC_PERF_P_UP,   &s_data.up_kbs },
        { "download_speed", PC_PERF_P_DOWN, &s_data.down_kbs },
        { "gpu",            PC_PERF_P_GPU,  &s_data.gpu_pct },
        { "disk",           PC_PERF_P_DISK, &s_data.disk_pct },
    };

    uint8_t present = 0;
    float v;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (json_number(s_buf, fields[i].key, &v)) {
            if (v < 0.0f) {
                v = 0.0f; /* clamp: some monitors report -1 while idle */
            }
            *fields[i].dst = v;
            present |= fields[i].bit;
        }
    }
    s_data.present = present;

    s_data.valid = true;
    s_data.connected = mqtt_manager_is_connected();
    s_data.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ever = true;
    s_frames++;

    if (s_frames == 1) {
        ESP_LOGI(TAG, "First PC frame via MQTT: cpu %.1f%% mem %.1f%% "
                 "up %.2f KB/s down %.2f KB/s gpu %.1f%% disk %.1f%%",
                 s_data.cpu_pct, s_data.mem_pct, s_data.up_kbs,
                 s_data.down_kbs, s_data.gpu_pct, s_data.disk_pct);
    }
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

esp_err_t pc_perf_mqtt_init(void)
{
    if (s_init_done) {
        return ESP_OK;
    }
    s_init_done = true;

    const char *topic = CONFIG_MQTT_PC_PERF_TOPIC;
    if (topic == NULL || topic[0] == '\0') {
        ESP_LOGW(TAG, "PC perf MQTT topic is empty — MQTT path disabled");
        return ESP_OK;
    }

    esp_err_t ret = mqtt_manager_subscribe(topic, 1, pc_perf_mqtt_msg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "subscribe failed for %s: %s", topic,
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "PC perf via MQTT: subscribing to \"%s\" (QoS 1)",
                 topic);
    }
    return ret;
}

void pc_perf_mqtt_get_data(pc_perf_data_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_data;
    /* Broker connectivity may change without any message; refresh it. */
    out->connected = mqtt_manager_is_connected();
}

bool pc_perf_mqtt_has_data(void)
{
    return s_ever;
}
