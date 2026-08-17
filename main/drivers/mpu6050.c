#include "mpu6050.h"
#include "i2c_bus.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mpu6050";

static esp_err_t read_who_am_i(uint16_t addr, uint8_t *who_am_i)
{
    i2c_master_dev_handle_t dev;
    esp_err_t ret = i2c_bus_add_device(addr, MPU6050_I2C_FREQ_HZ, &dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device 0x%02x failed: %s", addr, esp_err_to_name(ret));
        return ret;
    }

    uint8_t reg = MPU6050_REG_WHO_AM_I;
    ret = i2c_master_transmit_receive(dev, &reg, 1, who_am_i, 1,
                                      pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read WHO_AM_I @0x%02x failed: %s", addr, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

esp_err_t mpu6050_probe(void)
{
    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    /* Give the sensor (just powered) a moment to settle. */
    vTaskDelay(pdMS_TO_TICKS(100));

    static const uint16_t addrs[] = { MPU6050_I2C_ADDR, MPU6050_I2C_ADDR_ALT };

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        ret = i2c_bus_probe(addrs[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "no ACK at 0x%02x (%s)", addrs[i], esp_err_to_name(ret));
            continue;
        }

        uint8_t who_am_i = 0;
        if (read_who_am_i(addrs[i], &who_am_i) == ESP_OK &&
            who_am_i == MPU6050_WHO_AM_I_VALUE) {
            ESP_LOGI(TAG, "MPU6050 found: addr=0x%02x, WHO_AM_I=0x%02x",
                     addrs[i], who_am_i);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "ACK at 0x%02x but WHO_AM_I=0x%02x (not MPU6050?)",
                 addrs[i], who_am_i);
    }

    ESP_LOGE(TAG, "MPU6050 not found on I2C bus");
    return ESP_ERR_NOT_FOUND;
}
