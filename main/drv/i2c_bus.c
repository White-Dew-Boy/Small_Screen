#include "i2c_bus.h"

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus_handle;

/**
 * @brief 初始化 I2C 总线
 * 
 * 配置 I2C 总线的引脚（SDA、SCL）和参数，创建 I2C 主总线实例。
 * 如果总线已初始化，则直接返回 ESP_OK。
 * 
 * @return esp_err_t 
 *   - ESP_OK: 初始化成功
 *   - 其他值: 初始化失败的错误代码
 */
esp_err_t i2c_bus_init(void)
{
    if (s_bus_handle != NULL) {
        return ESP_OK;
    }

    gpio_config_t pin_cfg = {
        .pin_bit_mask = BIT64(I2C_BUS_SDA) | BIT64(I2C_BUS_SCL),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pin_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C pin config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_BUS_PORT,
        .sda_io_num        = I2C_BUS_SDA,
        .scl_io_num        = I2C_BUS_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", I2C_BUS_SDA, I2C_BUS_SCL);
    return ESP_OK;
}

/**
 * @brief 获取 I2C 总线句柄
 * 
 * @return i2c_master_bus_handle_t I2C 总线句柄（如果未初始化则为 NULL）
 */
i2c_master_bus_handle_t i2c_bus_get(void)
{
    return s_bus_handle;
}

/**
 * @brief 探测 I2C 设备
 * 
 * 尝试与指定地址的 I2C 设备进行通信，检查设备是否存在。
 * 
 * @param addr I2C 设备地址（7 位地址格式）
 * 
 * @return esp_err_t 
 *   - ESP_OK: 设备存在，响应了 ACK
 *   - ESP_ERR_INVALID_STATE: I2C 总线未初始化
 *   - 其他值: 探测失败或设备不响应
 */
esp_err_t i2c_bus_probe(uint16_t addr)
{
    if (s_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = i2c_master_probe(s_bus_handle, addr, 100);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Device ACK at address 0x%02x", addr);
    }
    return ret;
}

/**
 * @brief 添加 I2C 设备到总线
 * 
 * 将指定地址和速率的 I2C 设备配置添加到已初始化的总线中。
 * 
 * @param addr I2C 设备地址（7 位地址格式）
 * @param scl_speed SCL 时钟速度（单位：Hz）
 * @param dev 指向设备句柄的指针，用于接收新创建的设备句柄
 * 
 * @return esp_err_t 
 *   - ESP_OK: 设备添加成功
 *   - ESP_ERR_INVALID_STATE: I2C 总线未初始化
 *   - 其他值: 添加设备失败的错误代码
 */
esp_err_t i2c_bus_add_device(uint16_t addr, uint32_t scl_speed,
                             i2c_master_dev_handle_t *dev)
{
    if (s_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = scl_speed,
    };
    return i2c_master_bus_add_device(s_bus_handle, &dev_cfg, dev);
}
