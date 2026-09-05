/* Data-source selection for the PC-Perf page, persisted in NVS.
 * See pc_perf_src.h for the API. */

#include "pc_perf_src.h"

#include <stdbool.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pc_perf_src";

#define NVS_NS   "pcperf"
#define NVS_KEY  "src"

static pc_perf_src_t s_src = PC_PERF_SRC_MQTT;
static bool s_loaded = false;

/* Load once, lazily (NVS is initialized by wifi_manager_init() at boot). */
static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t v = (uint8_t)PC_PERF_SRC_MQTT;
    if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK &&
        v <= (uint8_t)PC_PERF_SRC_OFF) {
        s_src = (pc_perf_src_t)v;
    }
    nvs_close(h);
    s_loaded = true;
}

pc_perf_src_t pc_perf_src_get(void)
{
    if (!s_loaded) {
        load();
    }
    return s_src;
}

esp_err_t pc_perf_src_set(pc_perf_src_t src)
{
    if (src < PC_PERF_SRC_MQTT || src > PC_PERF_SRC_OFF) {
        return ESP_ERR_INVALID_ARG;
    }
    s_src = src;
    s_loaded = true;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(h, NVS_KEY, (uint8_t)src);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "PC data source set to %d", (int)src);
    return ret;
}
