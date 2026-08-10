#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hal/gpio_types.h"

/*============================================================================
 * Pin Configuration
 *============================================================================*/
#define FT6336_I2C_SCL   GPIO_NUM_17
#define FT6336_I2C_SDA   GPIO_NUM_18
#define FT6336_RST       GPIO_NUM_42
#define FT6336_INT       GPIO_NUM_2

/*============================================================================
 * I2C Parameters
 *============================================================================*/
#define FT6336_I2C_ADDR      0x38
#define FT6336_I2C_PORT      I2C_NUM_0
#define FT6336_I2C_FREQ_HZ   100000

/*============================================================================
 * Register Map
 *============================================================================*/
#define FT6336_REG_DEVIDE_MODE     0x00
#define FT6336_REG_TD_STATUS       0x02
/* Touch point 1: XH/XL/YH/YL = 0x03-0x06, point 2: 0x09-0x0C */
#define FT6336_REG_P1_XH           0x03
#define FT6336_REG_P1_YH           0x05
#define FT6336_REG_P2_XH           0x09
#define FT6336_REG_P2_YH           0x0B

/* Chip identification registers */
#define FT6336_REG_CIPHER_MID      0x9F
#define FT6336_REG_CIPHER_LOW      0xA0
#define FT6336_REG_CIPHER_HIGH     0xA3
#define FT6336_REG_G_MODE          0xA4
#define FT6336_REG_FOCALTECH_ID    0xA8

/* Threshold / timing config (optional) */
#define FT6336_REG_THGROUP         0x80
#define FT6336_REG_PERIODACTIVE    0x88

/*============================================================================
 * Panel Resolution + Modes
 *============================================================================*/
#define FT6336_MAX_X  240
#define FT6336_MAX_Y  320

#define FT6336_MODE_ACTIVE   0x00
#define FT6336_MODE_MONITOR  0x01

/*============================================================================
 * Data Structures
 *============================================================================*/
typedef struct {
    uint16_t x;
    uint16_t y;
} ft6336_point_t;

typedef struct {
    uint8_t       num_touches;  /* 0-2 */
    ft6336_point_t points[2];
} ft6336_touch_data_t;

/*============================================================================
 * API
 *============================================================================*/
/**
 * @brief Initialize FT6336G: reset pin, I2C bus, chip verification.
 *        Resets the chip via RST pin, waits for boot, then reads chip ID.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if chip not detected.
 */
esp_err_t ft6336_init(void);

void ft6336_read(ft6336_touch_data_t *data);

void ft6336_set_mode(uint8_t mode);

void ft6336_enter_monitor(void);
