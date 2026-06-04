#include "esp_err.h"

#define TCA9548A_ADDR 0x70       // 地址根据硬件接线改变
#define I2C_MASTER_NUM I2C_NUM_0 // 主I2C端口

void i2c_bus_mutex_init(void);
void i2c_bus_lock(void);
void i2c_bus_unlock(void);
esp_err_t tca9548a_select_channel(uint8_t channel);
esp_err_t tca9548a_disable_all_channels(void);