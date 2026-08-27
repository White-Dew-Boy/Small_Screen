#include "power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"

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

    gpio_config_t power_io_cfg = {
        .pin_bit_mask = BIT64(POWER_IMU_GPIO) | BIT64(POWER_TEMP_GPIO) |
                        BIT64(POWER_AUDIO_GPIO) | BIT64(POWER_LCD_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&power_io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MODULE POWER GPIO config failed: %s", esp_err_to_name(ret));
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

void power_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep (wake on KEY1 press)");

    /* Stop the radio so it does not keep draining during sleep */
    esp_wifi_stop();

    /* Power off every module that can be cut */
    for (int i = 0; i < POWER_ID_NUM; i++) {
        gpio_set_level(s_power_gpios[i], 0);
    }

    /* KEY1 (GPIO4) is the power button: idle low, pulled high when
     * pressed. Switch it to input with pull-down so the level is stable
     * and the press can be detected; the MP2636 latch is expected to
     * hold itself during sleep. */
    gpio_set_direction(POWER_KEY1_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(POWER_KEY1_GPIO, GPIO_PULLDOWN_ONLY);

    /* Wake when KEY1 is pressed (goes high) */
    esp_sleep_enable_ext1_wakeup((1ULL << POWER_KEY1_GPIO),
                                 ESP_EXT1_WAKEUP_ANY_HIGH);

    esp_deep_sleep_start();
    /* Not reached */
}
