#ifndef MPU6050_SENSOR_H
#define MPU6050_SENSOR_H

#include "esp_err.h"
#include <stdint.h>

/**
 * @brief Initialize the MPU6050 sensor on I2C_NUM_0
 *
 * It uses a safe I2C initialization method to avoid ESP_FAIL 
 * if the I2C bus is already installed by the LCD driver.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mpu6050_init(void);

/**
 * @brief Read acceleration data from MPU6050
 *
 * @param x Pointer to store X axis acceleration
 * @param y Pointer to store Y axis acceleration
 * @param z Pointer to store Z axis acceleration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z);

/**
 * @brief 轮询 MPU6050 状态并自动控制屏幕旋转
 * 
 * 建议在后台任务中周期性调用（例如每 500ms 一次）。
 */
void peripheral_mpu6050(void);

#endif
