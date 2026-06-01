#ifndef PCF8574_IO_H
#define PCF8574_IO_H

#include <pcf8574.h>
#include "esp_err.h"

// 定义 PCF8574 的 IO 预留分配
// P0-P2 给雾化器，P3-P7 给其他设备
typedef enum {
    EXT_IO_PIN_0 = 0,
    EXT_IO_PIN_1,
    EXT_IO_PIN_2,
    EXT_IO_PIN_3,
    EXT_IO_PIN_4,
    EXT_IO_PIN_5,
    EXT_IO_PIN_6,
    EXT_IO_PIN_7
} ext_io_pin_t;

// 全局变量，供其他模块访问
extern i2c_dev_t pcf8574_dev;

// 接口函数
esp_err_t pcf8574_io_init(void);
esp_err_t pcf8574_write_pin(ext_io_pin_t pin, uint32_t level);
esp_err_t pcf8574_read_pin(ext_io_pin_t pin, uint32_t *level);
esp_err_t pcf8574_write_port(uint8_t value);
esp_err_t pcf8574_read_port(uint8_t *value);

#endif
