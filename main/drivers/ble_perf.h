#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Latest PC performance snapshot received over BLE.
 *
 *        Written by the NimBLE host task (GATT write callback) and read by
 *        the LVGL refresh timer, so the members are volatile. Units match
 *        what the Windows tool sends (see tools/pc_perf_sender.py and the
 *        user's "PC mqtt tool"):
 *          cpu_pct  - CPU load in percent (0..100)
 *          mem_pct  - physical memory usage in percent (0..100)
 *          up_kbs   - network upload speed, KB/s
 *          down_kbs - network download speed, KB/s
 *        The following are optional (0.0 = not reported by the sender):
 *          gpu_pct  - GPU load in percent
 *          disk_pct - main disk usage in percent
 *          temp_c   - CPU temperature in Celsius
 *          fps      - frame rate
 *        valid is set once at least one complete binary frame arrived; it is
 *        cleared on disconnect. last_update_ms is the uptime (ms) when the
 *        last frame was received, so the UI can show stale data.
 */
typedef struct {
    volatile bool connected;      /* BLE link to the PC is up */
    volatile bool valid;          /* at least one data frame received */
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
 *        characteristic beb5483e-36e1-4688-b7f5-ea07361b26a8) and
 *        connectable advertising under the name "ESP32_PC_Monitor" — the
 *        exact UUIDs/name the Windows "PC mqtt tool" expects.
 *
 *        Wire format: fixed-length binary frame, 22 bytes little-endian:
 *               [0]  magic  0x50 ('P')
 *               [1]  version 0x01
 *               [2..3]   cpu   u16, 0.1 %
 *               [4..5]   mem   u16, 0.1 %
 *               [6..9]   up    u32, KB/s
 *               [10..13] down  u32, KB/s
 *               [14..15] gpu   u16, 0.1 %   (0 = not reported)
 *               [16..17] disk  u16, 0.1 %   (0 = not reported)
 *               [18..19] temp  i16, 0.1 C   (0 or -32768 = not reported)
 *               [20..21] fps   u16, ×1      (0 = not reported)
 *             Python pack: struct.pack('<BBHHIIHHhH', 0x50, 1, ...)
 *             The frame carries no timestamp — the ESP32 stamps reception.
 *             Needs ATT MTU >= 25; the driver requests 247 on connect.
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
