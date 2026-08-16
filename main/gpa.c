#include "gpa.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "gpa";

static const gpio_num_t s_gpa_gpios[GPA_ID_NUM] = {
    GPA_IMU_GPIO,
    GPA_TEMP_GPIO,
    GPA_AUDIO_GPIO,
    GPA_LCD_GPIO,
};

esp_err_t gpa_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(GPA_IMU_GPIO) | BIT64(GPA_TEMP_GPIO) |
                        BIT64(GPA_AUDIO_GPIO) | BIT64(GPA_LCD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < GPA_ID_NUM; i++) {
        gpio_set_level(s_gpa_gpios[i], 0);
    }

    ESP_LOGI(TAG, "GPA power driver initialized");
    return ESP_OK;
}

esp_err_t gpa_power_on(gpa_id_t id)
{
    if (id >= GPA_ID_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(s_gpa_gpios[id], 1);
}

esp_err_t gpa_power_off(gpa_id_t id)
{
    if (id >= GPA_ID_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    return gpio_set_level(s_gpa_gpios[id], 0);
}
