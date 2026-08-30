#include "ble_perf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Defined in the NimBLE port layer (ble_store_config.c) — used to give the
 * host a store backend (RAM-backed when NVS persist is disabled). */
void ble_store_config_init(void);

static const char *TAG = "ble_perf";

/* Advertising name: must match the Windows "PC mqtt tool" (.env
 * ESP32BLE_DEVICE_NAME). 17 chars — too long for the 31-byte advertising
 * packet together with the 128-bit service UUID, so the name goes into the
 * scan response (see pc_perf_advertise()). */
#define DEVICE_NAME "ESP32_PC_Monitor"

/* GATT service + write characteristic — exactly the UUIDs the Windows tool
 * uses (Arduino BLE_server example UUIDs). NimBLE stores 128-bit UUIDs in
 * little-endian wire order, i.e. the canonical string reversed. */
#define SVC_PC_PERF_UUID_STR  "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHR_PC_PERF_DATA_UUID_STR "beb5483e-36e1-4688-b7f5-ea07361b26a8"
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
    0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);
static const ble_uuid128_t chr_data_uuid = BLE_UUID128_INIT(
    0xa8, 0x26, 0x1b, 0x36, 0x07, 0xea, 0xf5, 0xb7,
    0x88, 0x46, 0xe1, 0x36, 0x3e, 0x48, 0xb5, 0xbe);

/* Wire protocol: fixed-length binary frame, 22 bytes little-endian:
 *       [0] magic 0x50, [1] version 0x01,
 *       [2..3] cpu u16 (0.1%), [4..5] mem u16 (0.1%),
 *       [6..9] up u32 (KB/s x 1000, i.e. B/s), [10..13] down u32 (same),
 *       [14..15] gpu u16 (0.1%), [16..17] disk u16 (0.1%),
 *       [18..19] temp i16 (0.1 C), [20..21] fps u16
 *     Sentinel: 0 in an optional field = not reported. For temp, the
 *     current sender uses 0 = not reported; -32768 is reserved as the
 *     "not reported" sentinel for a future sender that needs a real 0.0 C
 *     (then the check below must drop the ==0 case). The frame carries no
 *     timestamp: the ESP32 stamps reception time.
 * The 22-byte frame needs ATT MTU >= 25; pc_perf_init() requests 247 (see
 * CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU). */
#define FRAME_MAGIC   0x50
#define FRAME_VERSION 0x01
#define FRAME_SIZE    22
#define TEMP_NODATA   0      /* current sender: 0 = not reported */
#define TEMP_NODATA_SENTINEL (-32768) /* reserved future "not reported" */

/* Shared snapshot (written by the NimBLE host task, read by LVGL) */
static pc_perf_data_t s_data;

/* Fragment accumulator for binary frames (fixed FRAME_SIZE bytes) */
static uint8_t s_bin[FRAME_SIZE];
static int s_bin_len = 0;

static uint8_t s_own_addr_type;

/* Guard: pc_perf_init() may only act once (lazy start from the LVGL thread
 * the first time the PC-Perf page is shown). */
static bool s_init_done = false;

/*---------------------------------------------------------------------------
 * Frame parsing
 *---------------------------------------------------------------------------*/

/* Parse a FRAME_SIZE-byte binary frame (see the layout comment above).
 * Manual little-endian decoding keeps it portable. Called from the NimBLE
 * host task. */
static void parse_binary(const uint8_t *f)
{
    if (f[0] != FRAME_MAGIC || f[1] != FRAME_VERSION) {
        ESP_LOGW(TAG, "Bad binary frame header: %02x %02x", f[0], f[1]);
        return;
    }

    uint16_t cpu  = (uint16_t)(f[2] | (f[3] << 8));
    uint16_t mem  = (uint16_t)(f[4] | (f[5] << 8));
    uint32_t up   = (uint32_t)f[6] | ((uint32_t)f[7] << 8) |
                    ((uint32_t)f[8] << 16) | ((uint32_t)f[9] << 24);
    uint32_t down = (uint32_t)f[10] | ((uint32_t)f[11] << 8) |
                    ((uint32_t)f[12] << 16) | ((uint32_t)f[13] << 24);
    uint16_t gpu  = (uint16_t)(f[14] | (f[15] << 8));
    uint16_t disk = (uint16_t)(f[16] | (f[17] << 8));
    int16_t  temp = (int16_t)(uint16_t)(f[18] | (f[19] << 8));
    uint16_t fps  = (uint16_t)(f[20] | (f[21] << 8));

    s_data.cpu_pct = cpu / 10.0f;
    s_data.mem_pct = mem / 10.0f;
    /* Speeds are fixed-point KB/s x 1000 (i.e. B/s): divide by 1000. */
    s_data.up_kbs = (float)up / 1000.0f;
    s_data.down_kbs = (float)down / 1000.0f;
    s_data.gpu_pct = gpu / 10.0f;
    s_data.disk_pct = disk / 10.0f;
    /* temp sentinel: 0 (current sender) or -32768 (reserved) = not reported */
    s_data.temp_c = (temp == TEMP_NODATA || temp == TEMP_NODATA_SENTINEL)
                        ? 0.0f
                        : temp / 10.0f;
    s_data.fps = (float)fps;

    s_data.valid = true;
    s_data.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/* Append one received chunk to the frame buffer and parse completed frames.
 * A frame may arrive split over several GATT writes, so bytes are buffered
 * until FRAME_SIZE bytes are collected. Called from the NimBLE host task. */
static void frame_feed(const uint8_t *data, int len)
{
    for (int i = 0; i < len && s_bin_len < FRAME_SIZE; i++) {
        s_bin[s_bin_len++] = data[i];
        if (s_bin_len == FRAME_SIZE) {
            parse_binary(s_bin);
            s_bin_len = 0;
        }
    }
}

/*---------------------------------------------------------------------------
 * GATT
 *---------------------------------------------------------------------------*/

/* Write to the data characteristic: raw performance data, binary frame
 * (see frame_feed). */
static int pc_perf_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[FRAME_SIZE];

        if (len > sizeof(buf)) {
            len = sizeof(buf);
        }
        if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        frame_feed(buf, len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &chr_data_uuid.u,
                .access_cb = pc_perf_data_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

/* Register the GATT service table with the host (called once from init). */
static int gatt_svr_init_local(void)
{
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(gatt_svcs);
}

/*---------------------------------------------------------------------------
 * GAP
 *---------------------------------------------------------------------------*/

static void pc_perf_advertise(void);

static int pc_perf_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "PC connected");
            s_data.connected = true;
        } else {
            ESP_LOGW(TAG, "Connect failed (status=%d), re-advertising",
                     event->connect.status);
            pc_perf_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "PC disconnected (reason=%d)", event->disconnect.reason);
        s_data.connected = false;
        s_data.valid = false;
        pc_perf_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        pc_perf_advertise();
        return 0;

    default:
        return 0;
    }
}

/* Advertise: general discoverable, undirected connectable, forever.
 * Packet layout (each field ≤ 31 bytes):
 *   advertising  : flags + COMPLETE device name "ESP32_PC_Monitor" (21 B)
 *   scan response: 128-bit PC-Perf service UUID (18 B)
 * The name must be in the advertising packet itself: Windows' own scanner
 * and some tools ignore scan-response names. The 128-bit service UUID does
 * not need to be advertised at all for the PC tool (it discovers services
 * via GATT), it is only put in the scan response for generic BLE explorers
 * like nRF Connect. */
static void pc_perf_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    /* Use the compile-time constant directly (not ble_svc_gap_device_name())
     * so the ADV name can never fall back to the NimBLE default ("nimble")
     * no matter what state the GAP service name is in. */
    const char *name = DEVICE_NAME;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "ADV name: \"%s\" (%d bytes, complete=%d)",
             name, fields.name_len, fields.name_is_complete);

    /* Scan response: the 128-bit PC-Perf service UUID */
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = (ble_uuid128_t[]){ svc_uuid };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, pc_perf_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

/* Host sync: pick the address type and start advertising. */
static void pc_perf_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto failed: %d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    /* NimBLE stores the address little-endian; print it in canonical order */
    ESP_LOGI(TAG, "Advertising as \"%s\" (addr %02x:%02x:%02x:%02x:%02x:%02x)",
             DEVICE_NAME, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    pc_perf_advertise();
}

static void pc_perf_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

/* NimBLE host task (created by nimble_port_freertos_init) */
static void pc_perf_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/

void pc_perf_get_data(pc_perf_data_t *out)
{
    if (out == NULL) {
        return;
    }
    out->connected = s_data.connected;
    out->valid = s_data.valid;
    out->cpu_pct = s_data.cpu_pct;
    out->mem_pct = s_data.mem_pct;
    out->up_kbs = s_data.up_kbs;
    out->down_kbs = s_data.down_kbs;
    out->gpu_pct = s_data.gpu_pct;
    out->disk_pct = s_data.disk_pct;
    out->temp_c = s_data.temp_c;
    out->fps = s_data.fps;
    out->last_update_ms = s_data.last_update_ms;
}

esp_err_t pc_perf_init(void)
{
    int rc;

    if (s_init_done) {
        return ESP_OK;
    }
    s_init_done = true;

    /* nimble_port_init() releases classic-BT memory, initializes and enables
     * the BLE controller and starts the host port. NVS must already be
     * initialized (wifi_manager_init() does that in app_main). */
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = pc_perf_on_reset;
    ble_hs_cfg.sync_cb = pc_perf_on_sync;

    /* The 22-byte frame needs ATT MTU >= 25. Ask for 247 (NimBLE initiates
     * the MTU exchange on connect; the peer may agree to anything >= 25).
     * This mirrors CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247. */
    ble_att_set_preferred_mtu(247);

    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed: %d", rc);
        return ESP_FAIL;
    }

    /* Register the standard GAP (0x1800) / GATT (0x1801) services first —
     * host stacks like Windows enumerate characteristics through them.
     * (Both functions return void in this NimBLE version.) */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = gatt_svr_init_local();
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(pc_perf_host_task);

    ESP_LOGI(TAG, "BLE PC-Perf peripheral started (service %s, data %s)",
             SVC_PC_PERF_UUID_STR, CHR_PC_PERF_DATA_UUID_STR);
    return ESP_OK;
}
