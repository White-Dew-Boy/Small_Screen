#include "mpu6050.h"
#include "i2c_bus.h"
#include "esp_log.h"

static const char *TAG = "mpu6050";

/* Module-level device handle, added once in mpu6050_init() and reused by all reads/writes */
static i2c_master_dev_handle_t s_dev_handle;

/**
 * @brief Read a single register from MPU6050
 */
static esp_err_t mpu6050_read_reg(uint8_t reg, uint8_t *data)
{
    if (s_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register 0x%02x: %s", reg, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Write a single register to MPU6050
 */
static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t data)
{
    if (s_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t write_buf[2] = {reg, data};
    esp_err_t ret = i2c_master_transmit(s_dev_handle, write_buf, 2, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write register 0x%02x: %s", reg, esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Read multiple registers from MPU6050
 */
static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, uint8_t len)
{
    if (s_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read registers from 0x%02x: %s", reg, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t mpu6050_init(void)
{
    esp_err_t ret;

    // Probe device
    ret = i2c_bus_probe(MPU6050_ADDR_LOW);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 device not found at address 0x%02x", MPU6050_ADDR_LOW);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "MPU6050 found at address 0x%02x", MPU6050_ADDR_LOW);

    // Add the device to the shared I2C bus once; the handle is reused below
    ret = i2c_bus_add_device(MPU6050_ADDR_LOW, 100000, &s_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add MPU6050 device");
        return ret;
    }

    // Wake up the sensor (clear sleep bit)
    ret = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake up MPU6050");
        return ret;
    }

    // Set sample rate divider (1kHz / (1 + 9) = 100Hz)
    ret = mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV, 9);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set sample rate divider");
        return ret;
    }

    // Set gyroscope range to ±250°/s (0x00)
    ret = mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set gyroscope config");
        return ret;
    }

    // Set accelerometer range to ±2g (0x00)
    ret = mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set accelerometer config");
        return ret;
    }

    // Set low pass filter to 44Hz
    ret = mpu6050_write_reg(MPU6050_REG_CONFIG, 0x03);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set low pass filter");
        return ret;
    }

    ESP_LOGI(TAG, "MPU6050 initialized successfully");
    return ESP_OK;
}