#include "power.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "power";

static const gpio_num_t s_power_gpios[POWER_ID_NUM] = {
    POWER_IMU_GPIO,
    POWER_TEMP_GPIO,
    POWER_AUDIO_GPIO,
    POWER_LCD_GPIO,
};

esp_err_t power_init(void)
{
    gpio_config_t key1_cfg = {
        .pin_bit_mask = BIT64(POWER_KEY1_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&key1_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "KEY1 GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(POWER_KEY1_GPIO, 1);

    gpio_config_t gpa_cfg = {
        .pin_bit_mask = BIT64(POWER_IMU_GPIO) | BIT64(POWER_TEMP_GPIO) |
                        BIT64(POWER_AUDIO_GPIO) | BIT64(POWER_LCD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&gpa_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPA GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < POWER_ID_NUM; i++) {
        gpio_set_level(s_power_gpios[i], 0);
    }

    ESP_LOGI(TAG, "Power driver initialized (KEY1 latched, modules off)");
    return ESP_OK;
}

esp_err_t power_on(power_id_t id)
{
    if (id >= POWER_ID_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(s_power_gpios[id], 1);
}

esp_err_t power_off(power_id_t id)
{
    if (id >= POWER_ID_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(s_power_gpios[id], 0);
}
