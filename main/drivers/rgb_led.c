#include "rgb_led.h"

#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "rgb_led";

static led_strip_handle_t s_strip;

esp_err_t rgb_led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num          = RGB_LED_GPIO,
        .max_leds                = RGB_LED_NUM,
        .led_model               = LED_MODEL_WS2812,
        .color_component_format  = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out        = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = 10 * 1000 * 1000,
        .mem_block_symbols  = 64,
        .flags.with_dma     = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WS2812 strip initialized (gpio=%d, leds=%d)",
             RGB_LED_GPIO, RGB_LED_NUM);
    return ESP_OK;
}

esp_err_t rgb_led_set_pixel(uint32_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= RGB_LED_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    return led_strip_set_pixel(s_strip, index, r, g, b);
}

esp_err_t rgb_led_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret;
    for (uint32_t i = 0; i < RGB_LED_NUM; i++) {
        ret = led_strip_set_pixel(s_strip, i, r, g, b);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t rgb_led_refresh(void)
{
    return led_strip_refresh(s_strip);
}

esp_err_t rgb_led_clear(void)
{
    return led_strip_clear(s_strip);
}
