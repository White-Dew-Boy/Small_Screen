#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>

#define SHTC3_I2C_ADDR         0x70

#define SHTC3_CMD_SOFT_RESET   0x805D
#define SHTC3_CMD_READ_ID      0xEFC8

#define SHTC3_CMD_WAKEUP       0x3517
#define SHTC3_CMD_SLEEP        0xB098

/* Normal mode, clock stretching ON (recommended: sensor holds SCL until ready) */
#define SHTC3_CMD_MEAS_NORMAL_RH_FIRST_CS_ON      0x5C24
#define SHTC3_CMD_MEAS_NORMAL_T_FIRST_CS_ON       0x7CA2
/* Normal mode, clock stretching OFF (must poll or fixed-delay wait) */
#define SHTC3_CMD_MEAS_NORMAL_T_FIRST_CS_OFF      0x7866
#define SHTC3_CMD_MEAS_NORMAL_RH_FIRST_CS_OFF     0x58E0
/* Low-power mode */
#define SHTC3_CMD_MEAS_LOWPOWER_T_FIRST_CS_OFF    0x609C
#define SHTC3_CMD_MEAS_LOWPOWER_RH_FIRST_CS_OFF   0x401A

esp_err_t shtc3_init(void);
esp_err_t shtc3_getdata(uint16_t *humi, uint16_t *temp);
void shtc3_caculate_data(uint16_t *ori_humi, uint16_t *ori_temp, float *humi, float *temp);
