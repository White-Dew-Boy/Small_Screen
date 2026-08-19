#ifndef __MPU6050_H__
#define __MPU6050_H__

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MPU6050 I2C Address */
#define MPU6050_ADDR_LOW    0x68  // AD0 = 0
#define MPU6050_ADDR_HIGH   0x69  // AD0 = 1

/* MPU6050 Registers */
#define MPU6050_REG_SELF_TEST_X     0x0D
#define MPU6050_REG_SELF_TEST_Y     0x0E
#define MPU6050_REG_SELF_TEST_Z     0x0F
#define MPU6050_REG_SELF_TEST_A     0x10
#define MPU6050_REG_SMPLRT_DIV      0x19
#define MPU6050_REG_CONFIG          0x1A
#define MPU6050_REG_GYRO_CONFIG     0x1B
#define MPU6050_REG_ACCEL_CONFIG    0x1C
#define MPU6050_REG_FIFO_EN         0x23
#define MPU6050_REG_INT_PIN_CFG     0x37
#define MPU6050_REG_INT_ENABLE      0x38
#define MPU6050_REG_INT_STATUS      0x3A
#define MPU6050_REG_ACCEL_XOUT_H    0x3B
#define MPU6050_REG_ACCEL_XOUT_L    0x3C
#define MPU6050_REG_ACCEL_YOUT_H    0x3D
#define MPU6050_REG_ACCEL_YOUT_L    0x3E
#define MPU6050_REG_ACCEL_ZOUT_H    0x3F
#define MPU6050_REG_ACCEL_ZOUT_L    0x40
#define MPU6050_REG_TEMP_OUT_H      0x41
#define MPU6050_REG_TEMP_OUT_L      0x42
#define MPU6050_REG_GYRO_XOUT_H     0x43
#define MPU6050_REG_GYRO_XOUT_L     0x44
#define MPU6050_REG_GYRO_YOUT_H     0x45
#define MPU6050_REG_GYRO_YOUT_L     0x46
#define MPU6050_REG_GYRO_ZOUT_H     0x47
#define MPU6050_REG_GYRO_ZOUT_L     0x48
#define MPU6050_REG_PWR_MGMT_1      0x6B
#define MPU6050_REG_PWR_MGMT_2      0x6C
#define MPU6050_REG_WHO_AM_I        0x75

/* Accelerometer data structure */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} mpu6050_accel_t;

/* Gyroscope data structure */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} mpu6050_gyro_t;

/* Temperature data structure */
typedef struct {
    float temp;
} mpu6050_temp_t;

/**
 * @brief Initialize MPU6050 sensor
 * 
 * Uses MPU6050_ADDR_LOW (0x68) as the I2C address.
 * 
 * @return esp_err_t
 *   - ESP_OK: Initialization successful
 *   - ESP_ERR_NOT_FOUND: Device not found
 *   - Other error codes from I2C operations
 */
esp_err_t mpu6050_init(void);

#ifdef __cplusplus
}
#endif

#endif // __MPU6050_H__