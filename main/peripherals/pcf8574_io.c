#include "pcf8574_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "driver/i2c.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "PCF8574_IO";
static bool pcf8574_initialized = false;
i2c_dev_t pcf8574_dev;  // 全局定义

// I2C 引脚配置
#define I2C_MASTER_PORT  I2C_NUM_0
#define I2C_MASTER_SDA   CONFIG_I2C_MASTER_SDA
#define I2C_MASTER_SCL   CONFIG_I2C_MASTER_SCL
#define PCF8574_ADDR     0x27  

esp_err_t pcf8574_io_init(void)
{
    if (pcf8574_initialized) {
        return ESP_OK;
    }
    // 1. 初始化 I2C 总线
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };
    esp_err_t ret = i2c_param_config(I2C_MASTER_PORT, &conf);
    if (ret != ESP_OK) return ret;
    ret = i2c_driver_install(I2C_MASTER_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    // 2. 初始化 PCF8574 设备
    memset(&pcf8574_dev, 0, sizeof(i2c_dev_t));
    ret = pcf8574_init_desc(&pcf8574_dev, PCF8574_ADDR, I2C_MASTER_PORT, I2C_MASTER_SDA, I2C_MASTER_SCL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF8574 init failed");
        return ret;
    }

    // 3. 初始输出状态（设为全低，防止上电误触）
    ret = pcf8574_port_write(&pcf8574_dev, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF8574 port write failed");
        return ret;
    }

    ESP_LOGI(TAG, "PCF8574 initialized successfully");
    pcf8574_initialized = true;
    return ESP_OK;
}

esp_err_t pcf8574_write_pin(ext_io_pin_t pin, uint32_t level)
{
    // 使用库函数操作单个引脚（有并发风险）
    return pcf8574_set_level(&pcf8574_dev, (uint8_t)pin, level);
}

esp_err_t pcf8574_read_pin(ext_io_pin_t pin, uint32_t *level)
{
    return pcf8574_get_level(&pcf8574_dev, (uint8_t)pin, level);
}

esp_err_t pcf8574_write_port(uint8_t value)
{
    return pcf8574_port_write(&pcf8574_dev, value);
}

esp_err_t pcf8574_read_port(uint8_t *value)
{
    return pcf8574_port_read(&pcf8574_dev, value);
}