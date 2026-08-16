#include "key.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "key";

#define KEY_DEBOUNCE_SCANS  2

static const gpio_num_t s_key_gpios[KEY_ID_NUM] = {
    KEY2_GPIO,
    KEY3_GPIO,
};

typedef struct {
    uint8_t stable_level;
    uint8_t counter;
    bool    press_edge;
    bool    release_edge;
} key_state_t;

static key_state_t s_keys[KEY_ID_NUM];

esp_err_t key_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(KEY2_GPIO) | BIT64(KEY3_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (int i = 0; i < KEY_ID_NUM; i++) {
        s_keys[i].stable_level = 1;
        s_keys[i].counter = 0;
        s_keys[i].press_edge = false;
        s_keys[i].release_edge = false;
    }

    ESP_LOGI(TAG, "Key driver initialized");
    return ESP_OK;
}

void key_scan(void)
{
    for (int i = 0; i < KEY_ID_NUM; i++) {
        key_state_t *k = &s_keys[i];
        uint8_t raw = (uint8_t)gpio_get_level(s_key_gpios[i]);

        if (raw == k->stable_level) {
            k->counter = 0;
            continue;
        }

        if (++k->counter >= KEY_DEBOUNCE_SCANS) {
            if (k->stable_level == 1 && raw == 0) {
                k->press_edge = true;
            } else if (k->stable_level == 0 && raw == 1) {
                k->release_edge = true;
            }
            k->stable_level = raw;
            k->counter = 0;
        }
    }
}

bool key_is_pressed(key_id_t key)
{
    return s_keys[key].stable_level == 0;
}

bool key_pressed_edge(key_id_t key)
{
    bool edge = s_keys[key].press_edge;
    s_keys[key].press_edge = false;
    return edge;
}

bool key_released_edge(key_id_t key)
{
    bool edge = s_keys[key].release_edge;
    s_keys[key].release_edge = false;
    return edge;
}
