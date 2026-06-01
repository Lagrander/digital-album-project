/*
 * Header for AHT20 sensor helpers
 */
#ifndef AHT20_SENSOR_H
#define AHT20_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>

#ifndef I2C_MASTER_SCL_IO
#define I2C_MASTER_SCL_IO                                                      \
  CONFIG_I2C_MASTER_SCL /*!< gpio number for I2C master clock */
#endif
#ifndef I2C_MASTER_SDA_IO
#define I2C_MASTER_SDA_IO                                                      \
  CONFIG_I2C_MASTER_SDA /*!< gpio number for I2C master data  */
#endif
#ifndef I2C_MASTER_NUM
#define I2C_MASTER_NUM I2C_NUM_0 /*!< I2C port number for master dev */
#endif
#ifndef I2C_MASTER_FREQ_HZ
#define I2C_MASTER_FREQ_HZ                                                     \
  400000 // 100000                  /*!< I2C master clock frequency */
#endif

/**
 * Initialize the I2C bus and AHT20 sensor instance.
 */
esp_err_t i2c_sensor_aht20_init(void);

/**
 * Run the AHT20 peripheral demo: read and log values.
 */
void peripheral_aht20(void);

/**
 * Get the latest cached temperature and humidity.
 * Returns ESP_OK if valid data is available.
 */
esp_err_t aht20_get_latest(float *temp, float *hum);

/**
 * Processing task entry for AHT20.
 */
void aht20_process(void *arg);

#ifdef __cplusplus
}
#endif

#endif // AHT20_SENSOR_H
