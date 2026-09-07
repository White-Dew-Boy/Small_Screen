#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration (from materials/ESP32模块GPIO连接关系.md)
 *============================================================================*/
#define KEY2_GPIO   GPIO_NUM_5
#define KEY3_GPIO   GPIO_NUM_6

/*============================================================================
 * Data Structures
 *============================================================================*/
typedef enum {
    KEY_ID_2 = 0,
    KEY_ID_3,
    KEY_ID_NUM,
} key_id_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Configure all key pins as inputs with internal pull-up.
 *        Keys are active-low: pressed = 0, released = 1.
 * @return ESP_OK on success.
 */
esp_err_t key_init(void);

/**
 * @brief Run the debounce state machine for all keys.
 *        Call periodically (e.g. every 10 ms) to refresh states and edges.
 */
void key_scan(void);

/**
 * @brief Get the debounced current state of a key.
 * @param[in] key  Key index (KEY_ID_2..KEY_ID_3).
 * @return true if pressed (active-low), false otherwise.
 */
bool key_is_pressed(key_id_t key);

/**
 * @brief Get the "just pressed" edge since the last key_scan().
 * @param[in] key  Key index (KEY_ID_2..KEY_ID_3).
 * @return true if a press (falling) edge was detected, false otherwise.
 */
bool key_pressed_edge(key_id_t key);

/**
 * @brief Get the "just released" edge since the last key_scan().
 * @param[in] key  Key index (KEY_ID_2..KEY_ID_3).
 * @return true if a release (rising) edge was detected, false otherwise.
 */
bool key_released_edge(key_id_t key);
