#include "ft6336.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

static const char *TAG = "ft6336";

/*----------------------------------------------------------------------------
 * I2C bus scanner — probes every 7-bit address, logs what responds
 *----------------------------------------------------------------------------*/
static void i2c_scan(void)
{
    ESP_LOGI(TAG, "I2C bus scan (addr 0x01–0x7F):");
    int found = 0;
    for (uint8_t addr = 1; addr < 128; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(FT6336_I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  Device at 0x%02X", addr);
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  No devices found — check SDA/SCL wiring");
    }
}

/*----------------------------------------------------------------------------
 * Low-level I2C register read (writes reg addr, then reads len bytes)
 *----------------------------------------------------------------------------*/
static esp_err_t i2c_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(FT6336_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/*----------------------------------------------------------------------------
 * Hardware reset via RST pin
 *----------------------------------------------------------------------------*/
static void hardware_reset(void)
{
    gpio_set_level(FT6336_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(FT6336_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(FT6336_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(800));
}

/*============================================================================
 * PUBLIC API
 *============================================================================*/

esp_err_t ft6336_init(void)
{
    esp_err_t ret;

    /* ---- GPIO config for RST and INT ---- */
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

    /* ---- Hardware reset ---- */
    hardware_reset();

    /* ---- I2C master init (legacy driver, well-tested on FT6336) ---- */
    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = FT6336_I2C_SDA,
        .scl_io_num       = FT6336_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = FT6336_I2C_FREQ_HZ,
        .clk_flags        = 0,
    };
    ret = i2c_param_config(FT6336_I2C_PORT, &i2c_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %d", ret);
        return ret;
    }
    ret = i2c_driver_install(FT6336_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %d", ret);
        return ret;
    }

    /* ---- I2C bus scan for diagnostics ---- */
    i2c_scan();

    /* ---- Chip ID verification (retry up to 3 times) ---- */
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
