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
#include "host/ble_gap.h"
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

/* Wire protocol v2 (materials/ble_frame_parsing.md): fixed-length binary
 * frame, 23 bytes little-endian:
 *       [0] magic 0x50, [1] version 0x02,
 *       [2] flags u8 bitmask (bit N=1 => field N has data):
 *           bit0 cpu, bit1 mem, bit2 up, bit3 down, bit4 gpu,
 *           bit5 disk, bit6 temp, bit7 fps
 *       [3..4] cpu u16 (0.1%), [5..6] mem u16 (0.1%),
 *       [7..10] up u32 (KB/s x 1000, i.e. B/s), [11..14] down u32 (same),
 *       [15..16] gpu u16 (0.1%), [17..18] disk u16 (0.1%, disk utilization),
 *       [19..20] temp i16 (0.1 C, signed), [21..22] fps u16
 *     A field with flag bit = 0 carries NO data and must not be displayed;
 *     0 is a legal measured value since v2 (the old "0 = not reported"
 *     sentinels are gone). The frame carries no timestamp: the ESP32 stamps
 *     reception time.
 * The 23-byte frame needs ATT MTU >= 26; pc_perf_init() requests 247 (see
 * CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU). */
#define FRAME_MAGIC   0x50
#define FRAME_VERSION 0x02
#define FRAME_SIZE    23

/* Shared snapshot (written by the NimBLE host task, read by LVGL) */
static pc_perf_data_t s_data;

/* Fragment accumulator for binary frames (fixed FRAME_SIZE bytes) */
static uint8_t s_bin[FRAME_SIZE];
static int s_bin_len = 0;

static uint8_t s_own_addr_type;

/* Handle of the current BLE link (BLE_HS_CONN_HANDLE_NONE when none), so a
 * source switch away from BLE can actively terminate it. */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/* HCI reason sent to the peer when we terminate the link ourselves. */
#define BLE_LOCAL_TERM_REASON 0x13 /* Remote User Terminated Connection */

/* Advertising is request-based to save power: the UI asks for advertising
 * (pc_perf_advertise_start/stop) while the PC-Perf page is visible in BLE
 * mode. s_host_synced is set by the host-sync callback, s_advertising
 * tracks whether a connectable advertising run is currently active. */
static bool s_host_synced = false;
static bool s_adv_wanted = false;
static bool s_advertising = false;

/* Guard: pc_perf_init() may only act once (lazy start from the LVGL thread
 * the first time the PC-Perf page is shown). */
static bool s_init_done = false;

/*---------------------------------------------------------------------------
 * Frame parsing
 *---------------------------------------------------------------------------*/

/* Parse a FRAME_SIZE-byte binary frame (see the layout comment above).
 * Manual little-endian decoding keeps it portable. Called from the NimBLE
 * host task. Values of fields whose flag bit is 0 are meaningless (do not
 * display); `present` mirrors the flags byte 1:1. */
static void parse_binary(const uint8_t *f)
{
    if (f[0] != FRAME_MAGIC || f[1] != FRAME_VERSION) {
        ESP_LOGW(TAG, "Bad binary frame header: %02x %02x", f[0], f[1]);
        return;
    }

    uint8_t  flags = f[2];
    uint16_t cpu  = (uint16_t)(f[3] | (f[4] << 8));
    uint16_t mem  = (uint16_t)(f[5] | (f[6] << 8));
    uint32_t up   = (uint32_t)f[7] | ((uint32_t)f[8] << 8) |
                    ((uint32_t)f[9] << 16) | ((uint32_t)f[10] << 24);
    uint32_t down = (uint32_t)f[11] | ((uint32_t)f[12] << 8) |
                    ((uint32_t)f[13] << 16) | ((uint32_t)f[14] << 24);
    uint16_t gpu  = (uint16_t)(f[15] | (f[16] << 8));
    uint16_t disk = (uint16_t)(f[17] | (f[18] << 8));
    int16_t  temp = (int16_t)(uint16_t)(f[19] | (f[20] << 8)); /* signed */
    uint16_t fps  = (uint16_t)(f[21] | (f[22] << 8));

    s_data.present = flags;
    s_data.cpu_pct = cpu / 10.0f;
    s_data.mem_pct = mem / 10.0f;
    /* Speeds are fixed-point KB/s x 1000 (i.e. B/s): divide by 1000. */
    s_data.up_kbs = (float)up / 1000.0f;
    s_data.down_kbs = (float)down / 1000.0f;
    s_data.gpu_pct = gpu / 10.0f;
    s_data.disk_pct = disk / 10.0f;
    s_data.temp_c = temp / 10.0f; /* may be negative; no 0-sentinel anymore */
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
            s_conn_handle = event->connect.conn_handle;
            s_advertising = false; /* the stack stops advertising on connect */
        } else {
            ESP_LOGW(TAG, "Connect failed (status=%d)", event->connect.status);
            s_data.connected = false;
            if (s_adv_wanted) {
                pc_perf_advertise();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "PC disconnected (reason=%d)", event->disconnect.reason);
        s_data.connected = false;
        s_data.valid = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_advertising = false;
        /* Do NOT re-advertise unconditionally: only when the UI still
         * wants it (page visible, BLE source). Otherwise the radio stays
         * quiet until the page asks again. */
        if (s_adv_wanted) {
            pc_perf_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        if (s_adv_wanted) {
            pc_perf_advertise();
        }
        return 0;

    default:
        return 0;
    }
}

/* Start one connectable advertising run (if it finishes, the ADV_COMPLETE
 * event restarts it — but only while advertising is still wanted).
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
        s_advertising = false;
        return;
    }
    s_advertising = true;
}

/* Host sync: pick the address type and start advertising — but only when
 * the UI has requested it (page visible, BLE source). */
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

    s_host_synced = true;
    if (s_adv_wanted) {
        pc_perf_advertise();
    }
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
    out->present = s_data.present;
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

void pc_perf_advertise_start(void)
{
    s_adv_wanted = true;
    if (!s_host_synced || s_data.connected || s_advertising) {
        return; /* not ready yet, PC already linked, or already running */
    }
    pc_perf_advertise();
}

void pc_perf_advertise_stop(void)
{
    s_adv_wanted = false;
    if (!s_advertising) {
        return; /* nothing to stop (also when a PC is connected) */
    }
    s_advertising = false;
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_stop failed: %d", rc);
    }
}

/* Actively terminate an established BLE link (called when the data source
 * is switched away from BLE, e.g. to MQTT/Off). Idempotent: once the link
 * is gone (or was never there) this is a no-op; the DISCONNECT event also
 * clears the state. */
void pc_perf_disconnect(void)
{
    if (!s_data.connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ESP_LOGI(TAG, "Terminating BLE link (source no longer BLE)");
    int rc = ble_gap_terminate(s_conn_handle, BLE_LOCAL_TERM_REASON);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_terminate failed: %d", rc);
    }
    /* Clear immediately so a repeating UI timer does not retry; the
     * DISCONNECT event will confirm. */
    s_data.connected = false;
    s_data.valid = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
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

    /* The 23-byte v2 frame needs ATT MTU >= 26. Ask for 247 (NimBLE
     * initiates the MTU exchange on connect; the peer may agree to
     * anything >= 26). This mirrors CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU. */
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
