#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration (shared I2C bus, from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define I2C_BUS_SCL   GPIO_NUM_13
#define I2C_BUS_SDA   GPIO_NUM_12
#define I2C_BUS_PORT  I2C_NUM_0

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize the shared I2C master bus (idempotent).
 *        Configures SDA/SCL as open-drain with pull-up, then creates the bus.
 * @return ESP_OK on success or if already initialized.
 */
esp_err_t i2c_bus_init(void);

/**
 * @brief Get the shared I2C bus handle.
 * @return Bus handle, or NULL if not initialized.
 */
i2c_master_bus_handle_t i2c_bus_get(void);

/**
 * @brief Probe a 7-bit device address: returns ESP_OK if a device ACKs.
 * @param[in] addr  7-bit I2C address to probe.
 * @return ESP_OK if a device responds, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t i2c_bus_probe(uint16_t addr);

/**
 * @brief Add a device to the shared bus.
 * @param[in]  addr       7-bit device address.
 * @param[in]  scl_speed  SCL frequency in Hz for this device.
 * @param[out] dev        Device handle on success.
 * @return ESP_OK on success.
 */
esp_err_t i2c_bus_add_device(uint16_t addr, uint32_t scl_speed,
                             i2c_master_dev_handle_t *dev);
