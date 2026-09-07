/**
 * @file    shtc3.c
 * @brief   SHTC3 temperature & humidity sensor driver (Sensirion)
 *
 * Wiring: SCL=I2C_SCL_GPIO, SDA=I2C_SDA_GPIO, I2C addr=0x70
 */

#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "shtc3.h"
#include "i2c_bus.h"

static const char *TAG = "shtc3";

#define SHT_SHT3_I2C_TIMEOUT_MS  100   /* I2C timeout: fail fast when sensor is unpowered */

static i2c_master_dev_handle_t dev_handle;

/* ── I2C command ── */

/**
 * @brief  Send a 16-bit command to SHTC3 over I2C.
 *
 * With a 100ms timeout: if the sensor is unpowered, the call returns
 * ESP_ERR_TIMEOUT instead of blocking forever, so the main loop stays alive.
 *
 * @param  cmd  Command code (e.g. SHTC3_CMD_WAKEUP).
 * @return ESP_OK on success; I2C error code on failure.
 */
static esp_err_t shtc3_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = {cmd >> 8, cmd & 0xFF};
    esp_err_t ret = i2c_master_transmit(dev_handle, buf, sizeof(buf),
                                        pdMS_TO_TICKS(SHT_SHT3_I2C_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send cmd 0x%04X failed: %s (sensor may be unpowered)", cmd, esp_err_to_name(ret));
    }
    return ret;
}

/* ── CRC-8 (Sensirion polynomial 0x31) ── */

/**
 * @brief  Compute CRC-8/Maxim checksum (polynomial x^8+x^5+x^4+1 = 0x31).
 *
 * @param  data  Pointer to data buffer.
 * @param  len   Number of bytes.
 * @return 8-bit CRC value.
 */
static uint8_t shtc3_crc8(const uint8_t *data, size_t len)
{
    const uint8_t polynomial = 0x31;     /* x^8 + x^5 + x^4 + 1 */
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ polynomial;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* ── Public API ── */

/**
 * @brief  Initialize SHTC3 sensor.
 *         Probes device → adds to shared I2C bus → soft reset → sleep.
 *
 * @return ESP_OK on success; ESP_ERR_NOT_FOUND if probe fails; other error codes on failure.
 */
esp_err_t shtc3_init(void)
{
    esp_err_t ret;

    // Probe device
    ret = i2c_bus_probe(SHTC3_I2C_ADDR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SHTC3 device not found at address 0x%02x", SHTC3_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SHTC3 found at address 0x%02x", SHTC3_I2C_ADDR);

    /* Add device to the shared I2C bus once; the handle is reused by all transfers */
    ret = i2c_bus_add_device(SHTC3_I2C_ADDR, 100000, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHTC3 device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Soft reset then enter sleep */
    shtc3_send_cmd(SHTC3_CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(1));
    shtc3_send_cmd(SHTC3_CMD_SLEEP);

    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

/**
 * @brief  Read raw humidity and temperature from SHTC3.
 *
 * Sequence: wakeup → start measurement → wait 20ms → read 6 bytes → CRC check → sleep.
 *
 * @param[out] humi  Raw humidity value (0–65535).
 * @param[out] temp  Raw temperature value (0–65535).
 * @return ESP_OK on success;
 *         ESP_FAIL if I2C transfer fails;
 *         ESP_ERR_INVALID_CRC on CRC mismatch.
 */
esp_err_t shtc3_getdata(uint16_t *humi, uint16_t *temp)
{
    uint8_t rx_buf[6];
    esp_err_t ret;

    *humi = 0;
    *temp = 0;

    /* Wakeup */
    shtc3_send_cmd(SHTC3_CMD_WAKEUP);
    esp_rom_delay_us(250);

    /* Start measurement: normal mode, RH first, clock stretching off */
    ret = shtc3_send_cmd(SHTC3_CMD_MEAS_NORMAL_RH_FIRST_CS_OFF);
    if (ret != ESP_OK) goto sleep_and_exit;

    /* Wait for measurement (~12ms typ, 20ms for margin) */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Read 6 bytes: [RH_high, RH_low, RH_CRC, T_high, T_low, T_CRC] */
    ret = i2c_master_receive(dev_handle, rx_buf, sizeof(rx_buf),
                             pdMS_TO_TICKS(SHT_SHT3_I2C_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "receive failed: %s", esp_err_to_name(ret));
        goto sleep_and_exit;
    }

    /* CRC check */
    if (shtc3_crc8(rx_buf, 2) != rx_buf[2]) {
        ESP_LOGE(TAG, "humidity crc mismatch");
        ret = ESP_ERR_INVALID_CRC;
        goto sleep_and_exit;
    }
    if (shtc3_crc8(rx_buf + 3, 2) != rx_buf[5]) {
        ESP_LOGE(TAG, "temperature crc mismatch");
        ret = ESP_ERR_INVALID_CRC;
        goto sleep_and_exit;
    }

    *humi = (rx_buf[0] << 8) | rx_buf[1];
    *temp = (rx_buf[3] << 8) | rx_buf[4];
    ret = ESP_OK;

sleep_and_exit:
    shtc3_send_cmd(SHTC3_CMD_SLEEP);
    return ret;
}

/**
 * @brief  Convert raw ADC values to physical units.
 *
 * Formula (SHTC3 datasheet):
 *   RH(%) = 100 * (S_RH / 65535)
 *   T(°C) = -45 + 175 * (S_T / 65535)
 *
 * @param[in]  ori_humi  Raw humidity from shtc3_getdata().
 * @param[in]  ori_temp  Raw temperature from shtc3_getdata().
 * @param[out] humi      Relative humidity in %RH.
 * @param[out] temp      Temperature in °C.
 */
void shtc3_caculate_data(uint16_t *ori_humi, uint16_t *ori_temp, float *humi, float *temp)
{
    *humi = 100.0f * ((float)*ori_humi / 65535.0f);
    *temp = -45.0f + 175.0f * ((float)*ori_temp / 65535.0f);
}
