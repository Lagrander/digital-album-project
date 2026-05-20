/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "aht20_sensor.h"
#include "esp_system.h"
#include "esp_log.h"
#include "aht20.h"

static const char *TAG = "aht20 ";

static i2c_bus_handle_t i2c_bus;
static aht20_dev_handle_t aht20 = NULL;
static bool aht20_initialized = false;

esp_err_t i2c_sensor_aht20_init(void)
{
    if (aht20_initialized) {
        return ESP_OK;
    }

    const i2c_config_t i2c_bus_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    i2c_bus = i2c_bus_create(I2C_MASTER_NUM, &i2c_bus_conf);

    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "i2c_bus create returned NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    aht20_i2c_config_t i2c_conf = {
        .bus_inst = i2c_bus,
        .i2c_addr = AHT20_ADDRRES_0,
    };

    esp_err_t ret = aht20_new_sensor(&i2c_conf, &aht20);
    if (ret != ESP_OK || aht20 == NULL) {
        ESP_LOGE(TAG, "AHT20 create failed: %s", esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    aht20_initialized = true;
    return ESP_OK;
}

void peripheral_aht20()
{
    esp_err_t ret = ESP_OK;
    uint32_t temperature_raw;
    uint32_t humidity_raw;
    float temperature;
    float humidity;
    
    /* 读取温湿度 */
    ret = aht20_read_temperature_humidity(aht20, &temperature_raw, &temperature, &humidity_raw, &humidity);
    
    if(ret == ESP_OK) {
        ESP_LOGI(TAG, "%-20s: %2.2f %%", "humidity is", humidity);
        ESP_LOGI(TAG, "%-20s: %2.2f degC", "temperature is", temperature);
    } else {
        ESP_LOGE(TAG, "Failed to read temperature and humidity: %s", esp_err_to_name(ret));
    }
}

void aht20_process(void *arg)
{
    /* 默认行为：执行一次读取并退出。
        后续可扩展为循环运行或事件驱动。 */
    peripheral_aht20();
    vTaskDelete(NULL);
}