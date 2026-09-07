#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Presence bits for pc_perf_data_t.present: a bit set means the field
 * carries a valid measured value (0 is a legal value). Bit numbering
 * matches the v2 BLE frame flags byte (see below) so the driver can copy
 * it 1:1. */
#define PC_PERF_P_CPU   0x01
#define PC_PERF_P_MEM   0x02
#define PC_PERF_P_UP    0x04
#define PC_PERF_P_DOWN  0x08
#define PC_PERF_P_GPU   0x10
#define PC_PERF_P_DISK  0x20
#define PC_PERF_P_TEMP  0x40
#define PC_PERF_P_FPS   0x80

/**
 * @brief Latest PC performance snapshot (shared by the BLE and MQTT
 *        transports; the UI page shows whichever source is selected).
 *
 *        Written by the transport task (NimBLE host task / esp-mqtt task)
 *        and read by the LVGL refresh timer, so the members are volatile.
 *
 *        Units: cpu_pct / mem_pct / gpu_pct in percent, up_kbs / down_kbs
 *        in KB/s (the wire fields carry KB/s x 1000, i.e. B/s; drivers
 *        divide by 1000), temp_c in Celsius (may be negative), fps ×1.
 *
 *        `present` holds the PC_PERF_P_* bits of the last frame: a field
 *        whose bit is 0 has NO data — the UI must not display it (0 is a
 *        legal measured value since the v2 BLE protocol; absence is only
 *        expressed through the bit).
 *
 *        valid is set once at least one complete frame arrived; it is
 *        cleared on BLE disconnect. last_update_ms is the uptime (ms) when
 *        the last frame was received, so the UI can show stale data.
 */
typedef struct {
    volatile bool connected;      /* link up: BLE link / MQTT broker */
    volatile bool valid;          /* at least one data frame received */
    volatile uint8_t present;     /* PC_PERF_P_* bits of the last frame */
    volatile float cpu_pct;
    volatile float mem_pct;
    volatile float up_kbs;
    volatile float down_kbs;
    volatile float gpu_pct;
    volatile float disk_pct;
    volatile float temp_c;
    volatile float fps;
    volatile uint32_t last_update_ms;
} pc_perf_data_t;

/**
 * @brief Start the BLE (NimBLE) peripheral: GATT server with the PC-Perf
 *        service (UUID 4fafc201-1fb5-459e-8fcc-c5c9c331914b, write
 *        characteristic beb5483e-36e1-4688-b7f5-ea07361b26a8) — the exact
 *        UUIDs the Windows "PC mqtt tool" expects.
 *
 *        Advertising does NOT start automatically: request it with
 *        pc_perf_advertise_start() when the PC-Perf page is shown (the UI
 *        does this in BLE source mode) and stop it with
 *        pc_perf_advertise_stop() when the page hides, so the radio does
 *        not beacon pointlessly.
 *
 *        Wire format (v2): fixed-length binary frame, 23 bytes
 *        little-endian (materials/ble_frame_parsing.md):
 *               [0]  magic  0x50 ('P')
 *               [1]  version 0x02
 *               [2]  flags  u8 bitmask: bit N=1 => field N has data
 *                    (bit0 cpu, bit1 mem, bit2 up, bit3 down, bit4 gpu,
 *                     bit5 disk, bit6 temp, bit7 fps)
 *               [3..4]   cpu   u16, 0.1 %
 *               [5..6]   mem   u16, 0.1 %
 *               [7..10]  up    u32, KB/s x 1000 (i.e. B/s)
 *               [11..14] down  u32, KB/s x 1000 (i.e. B/s)
 *               [15..16] gpu   u16, 0.1 %
 *               [17..18] disk  u16, 0.1 %   (disk utilization, busy %)
 *               [19..20] temp  i16, 0.1 C   (signed)
 *               [21..22] fps   u16, ×1
 *             A field with its flag bit = 0 carries no data and must not be
 *             displayed — 0 itself is a legal measurement (v1's "0 = not
 *             reported" is gone).
 *             Python pack: struct.pack('<BBBHHIIHHhH', 0x50, 2, flags, ...)
 *             The frame carries no timestamp — the ESP32 stamps reception.
 *             Needs ATT MTU >= 26; the driver requests 247 on connect.
 *
 *        LAZY INIT: internal RAM is tight on this board (WiFi + MQTT/TLS +
 *        BT controller), so this must NOT be called at boot — it is invoked
 *        the first time the PC Perf page becomes visible, keeping the boot
 *        memory footprint identical to the pre-BLE firmware so the LVGL
 *        task (16 KB internal stack) always starts. Safe to call from the
 *        LVGL thread; the function is guarded and only acts once.
 *
 *        Non-fatal: on failure the UI page simply shows "waiting for PC".
 *        Must be called after NVS init (wifi_manager_init() does that).
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t pc_perf_init(void);

/**
 * @brief Copy the latest PC performance snapshot out of the BLE driver.
 *        Safe to call from any task.
 */
void pc_perf_get_data(pc_perf_data_t *out);

/**
 * @brief Request connectable advertising to run.
 *
 * Advertising is NOT always-on: after pc_perf_init() the device stays
 * silent until somebody requests advertising. This marks advertising as
 * wanted and starts it right away when the host is synced, no PC is
 * connected and advertising is not already running. The UI calls this
 * while the PC-Perf page is visible in BLE mode.
 *
 * Safe to call from any task (NimBLE API).
 */
void pc_perf_advertise_start(void);

/**
 * @brief Withdraw the advertising request and stop advertising.
 *
 * Idempotent. A connected PC is left alone (only the connectable
 * advertising is stopped). The UI calls this when the page is hidden or
 * the data source is not BLE, so the radio does not beacon pointlessly.
 *
 * Safe to call from any task (NimBLE API).
 */
void pc_perf_advertise_stop(void);

/**
 * @brief Actively disconnect an established BLE link.
 *
 * Idempotent no-op when no PC is connected. The UI calls this when the
 * data source is switched away from BLE (MQTT/Off), so the PC sees the
 * link go down immediately instead of staying "connected" forever.
 *
 * Safe to call from any task (NimBLE API).
 */
void pc_perf_disconnect(void);
