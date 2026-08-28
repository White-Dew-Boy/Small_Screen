#pragma once

#include "esp_err.h"
#include "hal/uart_types.h"
#include <stdbool.h>
#include <stdint.h>

/* JY901S 9-axis IMU (WITMOTION 维特智能), UART standard protocol
 *
 * 接线（JY901S 接到 ESP32-S3 模组 TXD0/RXD0 = UART0 = GPIO43/44）：
 *   JY901S TX  -> ESP32-S3 RXD0 (GPIO44)
 *   JY901S RX  <- ESP32-S3 TXD0 (GPIO43)
 *   JY901S VCC -> 3.3V ~ 5V      GND -> GND
 *
 * 注意：UART0 也是 ESP-IDF 日志的默认输出口，GPIO43 上会混有日志字节，
 * 模块会忽略非 0x55 开头的帧，不影响工作。若想彻底隔离日志，把 console
 * 切到 USB-Serial-JTAG（sdkconfig.defaults 加 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y）。
 */

#define JY901S_UART_NUM      UART_NUM_0
#define JY901S_TX_GPIO       43     /* ESP32-S3 TXD0  -> JY901S RX */
#define JY901S_RX_GPIO       44     /* ESP32-S3 RXD0  <- JY901S TX */
#define JY901S_BAUDRATE      9600   /* 模块出厂波特率（JY901S 多为 9600） */

/* 维特智能 JY901S 分辨率（±16g / ±2000°/s / ±180° 默认量程） */
#define JY901S_ACCEL_SCALE   0.0005f /* 加速度 g / LSB      */
#define JY901S_GYRO_SCALE    0.05f   /* 角速度 deg/s / LSB  */
#define JY901S_ANGLE_SCALE   0.005f  /* 角度   deg / LSB    */
#define JY901S_TEMP_SCALE    0.01f   /* 温度   degC / LSB   */

typedef struct {
    float roll;              /* 横滚角 deg, ±180            */
    float pitch;             /* 俯仰角 deg,  ±90            */
    float yaw;               /* 航向角 deg, 0~360           */
    float ax, ay, az;        /* 加速度 g                    */
    float wx, wy, wz;        /* 角速度 deg/s                */
    float temp;              /* 温度 degC                   */
    uint32_t last_update_ms; /* 最近一次有效角度帧时间戳 (esp_timer, ms) */
} jy901s_data_t;

/* 通信诊断统计（调试用） */
typedef struct {
    uint32_t rx_bytes;   /* UART 收到的原始字节总数        */
    uint32_t frames_ok;  /* 校验通过的帧数                 */
    uint32_t frames_bad; /* 校验失败的帧数                 */
    uint32_t resyncs;    /* 帧头/类型不匹配重同步次数       */
} jy901s_stats_t;

/**
 * @brief 初始化 UART 并启动后台解析任务。
 * @return ESP_OK 成功；失败返回对应错误码。
 */
esp_err_t jy901s_init(void);

/**
 * @brief 获取最新一帧解析结果（线程安全，任务写入/此处读取）。
 * @param[out] out 输出结构体，不可为 NULL。
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t jy901s_get_data(jy901s_data_t *out);

/**
 * @brief 查询模块是否在线（收到过数据且最近 timeout_ms 内有有效帧）。
 * @param timeout_ms 超时窗口，如 1000。
 * @return true 在线。
 */
bool jy901s_is_online(uint32_t timeout_ms);

/**
 * @brief 获取通信诊断统计（接线/波特率排查用）。
 * @param[out] out 输出统计结构体，不可为 NULL。
 */
esp_err_t jy901s_get_stats(jy901s_stats_t *out);

/**
 * @brief 获取最近收到的原始字节（调试用，环形缓冲）。
 * @param[out] buf    输出缓冲，至少 JY901S_RAW_DUMP_SIZE 字节。
 * @param[in]  buf_size 缓冲大小。
 * @param[out] out_len 实际写入字节数（最多 JY901S_RAW_DUMP_SIZE）。
 */
#define JY901S_RAW_DUMP_SIZE 64
esp_err_t jy901s_get_raw_dump(uint8_t *buf, uint32_t buf_size, uint32_t *out_len);
