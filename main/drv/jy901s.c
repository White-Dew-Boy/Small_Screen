/**
 * @file    jy901s.c
 * @brief   JY901S 9-axis IMU driver (WITMOTION standard UART protocol)
 *
 * The module streams 11-byte frames continuously at 115200 baud:
 *
 *     0x55 | type | d0 d1 d2 d3 d4 d5 d6 d7 | SUM
 *
 *   - type 0x51: accel  (ax, ay, az) + temperature
 *   - type 0x52: gyro   (wx, wy, wz) + temperature
 *   - type 0x53: angle  (roll, pitch, yaw) + temperature
 *   - type 0x54: mag    (not used here)
 *   - SUM = low byte of the sum of the previous 10 bytes
 *
 * All values are int16 little-endian; scale factors in jy901s.h.
 * A FreeRTOS task owns the byte-stream parsing and updates a shared
 * sample struct; readers use jy901s_get_data() under a spinlock.
 */

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jy901s.h"

static const char *TAG = "jy901s";

#define JY901S_RX_BUF_SIZE  2048
#define JY901S_FRAME_LEN    11
#define JY901S_TASK_STACK   4096
#define JY901S_TASK_PRIO    5

/* Frame type bytes (second byte after the 0x55 header) */
#define JY901S_ID_ACCEL     0x51
#define JY901S_ID_GYRO      0x52
#define JY901S_ID_ANGLE     0x53
#define JY901S_ID_MAG       0x54

static jy901s_data_t s_data;                  /* latest parsed sample */
static jy901s_stats_t s_stats;                /* comm diagnostics     */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_online = false;

/* Ring buffer of raw RX bytes for debugging */
static uint8_t  s_raw_buf[JY901S_RAW_DUMP_SIZE];
static uint32_t s_raw_idx = 0;

/* ── frame parser ── */

/**
 * @brief  Validate and apply one complete 11-byte frame.
 * @param  f  Frame buffer: [0x55][type][d0..d7][SUM].
 */
static void jy901s_parse_frame(const uint8_t *f)
{
    /* Checksum: SUM must equal the low byte of the sum of bytes 0..9 */
    uint8_t sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += f[i];
    }
    if (sum != f[10]) {
        portENTER_CRITICAL(&s_mux);
        s_stats.frames_bad++;
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "checksum fail: type=0x%02X sum=0x%02X calc=0x%02X",
                 f[1], f[10], sum);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_stats.frames_ok++;
    portEXIT_CRITICAL(&s_mux);

    /* Four little-endian int16 values (d0..d7) */
    int16_t v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = (int16_t)(f[2 + i * 2] | (f[3 + i * 2] << 8));
    }

    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    portENTER_CRITICAL(&s_mux);
    switch (f[1]) {
    case JY901S_ID_ACCEL:
        s_data.ax = v[0] * JY901S_ACCEL_SCALE;
        s_data.ay = v[1] * JY901S_ACCEL_SCALE;
        s_data.az = v[2] * JY901S_ACCEL_SCALE;
        /* Real temperature is carried in the accel frame (verified on
         * hardware: the angle frame's d6..d7 holds a constant 0x4708
         * on this firmware, while accel frame temp tracks ambient). */
        s_data.temp = v[3] * JY901S_TEMP_SCALE;
        break;

    case JY901S_ID_GYRO:
        s_data.wx = v[0] * JY901S_GYRO_SCALE;
        s_data.wy = v[1] * JY901S_GYRO_SCALE;
        s_data.wz = v[2] * JY901S_GYRO_SCALE;
        break;

    case JY901S_ID_ANGLE:
        s_data.roll  = v[0] * JY901S_ANGLE_SCALE;
        s_data.pitch = v[1] * JY901S_ANGLE_SCALE;
        s_data.yaw   = v[2] * JY901S_ANGLE_SCALE;
        if (s_data.yaw < 0.0f) {
            s_data.yaw += 360.0f;   /* module reports -180..180, normalize to 0..360 */
        }
        s_data.last_update_ms = now_ms;
        s_online = true;
        break;

    case JY901S_ID_MAG:
        /* magnetic field frame - not needed, ignore */
        break;

    default:
        break;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* ── RX parser task ── */

/**
 * @brief  Byte-stream state machine: hunt for 0x55 header, collect an
 *         11-byte frame, then validate it via jy901s_parse_frame().
 */
static void jy901s_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    uint8_t frame[JY901S_FRAME_LEN];
    uint8_t fpos = 0;

    while (1) {
        const int len = uart_read_bytes(JY901S_UART_NUM, buf, sizeof(buf),
                                        pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        portENTER_CRITICAL(&s_mux);
        s_stats.rx_bytes += (uint32_t)len;
        for (int i = 0; i < len; i++) {
            s_raw_buf[s_raw_idx] = buf[i];
            s_raw_idx = (s_raw_idx + 1) % JY901S_RAW_DUMP_SIZE;
        }
        portEXIT_CRITICAL(&s_mux);

        for (int i = 0; i < len; i++) {
            const uint8_t b = buf[i];

            if (fpos == 0) {
                if (b == 0x55) {
                    frame[fpos++] = b;
                }
            } else if (fpos == 1) {
                if (b >= 0x50 && b <= 0x59) {   /* valid frame type byte */
                    frame[fpos++] = b;
                } else {                        /* bad type, resync */
                    fpos = 0;
                    portENTER_CRITICAL(&s_mux);
                    s_stats.resyncs++;
                    portEXIT_CRITICAL(&s_mux);
                }
            } else {
                frame[fpos++] = b;
                if (fpos == JY901S_FRAME_LEN) {
                    jy901s_parse_frame(frame);
                    fpos = 0;
                }
            }
        }
    }
}

/* ── public API ── */

esp_err_t jy901s_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = JY901S_BAUDRATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(JY901S_UART_NUM, JY901S_RX_BUF_SIZE,
                                            0, 0, NULL, 0),
                        TAG, "uart driver install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(JY901S_UART_NUM, &cfg),
                        TAG, "uart param config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(JY901S_UART_NUM, JY901S_TX_GPIO,
                                     JY901S_RX_GPIO, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart set pin failed");
    ESP_RETURN_ON_ERROR(uart_flush_input(JY901S_UART_NUM),
                        TAG, "uart flush failed");

    xTaskCreate(jy901s_task, "jy901s_task", JY901S_TASK_STACK, NULL,
                JY901S_TASK_PRIO, NULL);

    ESP_LOGI(TAG, "initialized: UART%d TX=GPIO%d RX=GPIO%d @ %d baud",
             JY901S_UART_NUM, JY901S_TX_GPIO, JY901S_RX_GPIO, JY901S_BAUDRATE);
    return ESP_OK;
}

esp_err_t jy901s_get_data(jy901s_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_data;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool jy901s_is_online(uint32_t timeout_ms)
{
    bool online;
    uint32_t last;
    portENTER_CRITICAL(&s_mux);
    online = s_online;
    last   = s_data.last_update_ms;
    portEXIT_CRITICAL(&s_mux);

    if (!online) {
        return false;
    }
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (now - last) <= timeout_ms;  /* unsigned subtraction is wrap-safe */
}

esp_err_t jy901s_get_stats(jy901s_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

esp_err_t jy901s_get_raw_dump(uint8_t *buf, uint32_t buf_size, uint32_t *out_len)
{
    if (buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t n = (buf_size < JY901S_RAW_DUMP_SIZE)
                           ? buf_size : JY901S_RAW_DUMP_SIZE;
    portENTER_CRITICAL(&s_mux);
    for (uint32_t i = 0; i < n; i++) {
        /* newest first: read backwards from the write cursor */
        buf[i] = s_raw_buf[(s_raw_idx + JY901S_RAW_DUMP_SIZE - 1 - i)
                           % JY901S_RAW_DUMP_SIZE];
    }
    *out_len = n;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}
