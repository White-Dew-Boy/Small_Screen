#include "ft6336.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

static const char *TAG = "ft6336";

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t  dev_handle;

static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle,
                                       &reg, 1, data, len,
                                       pdMS_TO_TICKS(100));
}

static void hardware_reset(void)
{
    gpio_set_level(FT6336_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(FT6336_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(FT6336_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(800));
}

esp_err_t ft6336_init(void)
{
    esp_err_t ret;

    gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(FT6336_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);

    gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(FT6336_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    hardware_reset();

    gpio_config_t i2c_pin_cfg = {
        .pin_bit_mask = BIT64(FT6336_I2C_SDA) | BIT64(FT6336_I2C_SCL),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&i2c_pin_cfg);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port     = FT6336_I2C_PORT,
        .sda_io_num   = FT6336_I2C_SDA,
        .scl_io_num   = FT6336_I2C_SCL,
        .clk_source   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ret = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %d", ret);
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = FT6336_I2C_ADDR,
        .scl_speed_hz    = FT6336_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed: %d", ret);
        return ret;
    }

    uint8_t chip_id = 0;
    ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ret = i2c_read_reg(FT6336_REG_FOCALTECH_ID, &chip_id, 1);
        if (ret == ESP_OK && chip_id == 0x11) {
            break;
        }
        ESP_LOGW(TAG, "Chip probe attempt %d: ret=%d, id=0x%02x", attempt + 1, ret, chip_id);
    }
    if (ret != ESP_OK || chip_id != 0x11) {
        ESP_LOGE(TAG, "FT6336 not found (chip_id=0x%02x, ret=%d)", chip_id, ret);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "FT6336G initialized (I2C addr=0x%02x, chip_id=0x%02x)",
             FT6336_I2C_ADDR, chip_id);
    return ESP_OK;
}

void ft6336_read(ft6336_touch_data_t *data)
{
    data->num_touches = 0;

    uint8_t status;
    if (i2c_read_reg(FT6336_REG_TD_STATUS, &status, 1) != ESP_OK) {
        return;
    }

    uint8_t touches = status & 0x0F;
    if (touches == 0 || touches > 2) {
        return;
    }
    data->num_touches = touches;

    uint8_t buf[6];
    if (i2c_read_reg(FT6336_REG_P1_XH, buf, 6) == ESP_OK) {
        data->points[0].x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
        data->points[0].y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    }

    if (touches >= 2) {
        if (i2c_read_reg(FT6336_REG_P2_XH, buf, 6) == ESP_OK) {
            data->points[1].x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
            data->points[1].y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
        }
    }
}

void ft6336_set_mode(uint8_t mode)
{
    uint8_t buf[2] = { FT6336_REG_G_MODE, mode };
    i2c_master_transmit(dev_handle, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

void ft6336_enter_monitor(void)
{
    ft6336_set_mode(FT6336_MODE_MONITOR);
}
