/*
 * Header for AHT20 sensor helpers
 */
#ifndef AHT20_SENSOR_H
#define AHT20_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"

#define I2C_MASTER_SCL_IO   CONFIG_I2C_MASTER_SCL   /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO   CONFIG_I2C_MASTER_SDA   /*!< gpio number for I2C master data  */
#define I2C_MASTER_NUM      I2C_NUM_0               /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ  100000                  /*!< I2C master clock frequency */

/**
 * Initialize the I2C bus and AHT20 sensor instance.
 * Exposed so a task or other module can initialize the peripheral.
 */
esp_err_t i2c_sensor_aht20_init(void);

/**
 * Run the AHT20 peripheral demo: read and log values.
 */
void peripheral_aht20(void);

/**
 * Processing task entry for AHT20. Designed to be started as a FreeRTOS task.
 * The implementation currently runs a single read and deletes the task.
 */
void aht20_process(void *arg);

#ifdef __cplusplus
}
#endif

#endif // AHT20_SENSOR_H
