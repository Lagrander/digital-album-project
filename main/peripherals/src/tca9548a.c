#include "tca9548a.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/interrupts.h"

static SemaphoreHandle_t s_i2c_bus_mutex = NULL;

void i2c_bus_mutex_init(void) { s_i2c_bus_mutex = xSemaphoreCreateMutex(); }

void i2c_bus_lock(void) {
  if (s_i2c_bus_mutex != NULL) {
    if (xSemaphoreTake(s_i2c_bus_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      ESP_LOGW("I2C_BUS", "I2C mutex timeout (100ms) — possible deadlock or slow transaction");
    }
  }
}

void i2c_bus_unlock(void) {
  if (s_i2c_bus_mutex != NULL) {
    xSemaphoreGive(s_i2c_bus_mutex);
  }
}

/**
 * @brief 强行切换 TCA9548A 的物理通道
 *
 */
esp_err_t tca9548a_select_channel(uint8_t channel) {
  // 1. 安全校验：只接受 0~7 之间的通道号
  if (channel > 7) {
    ESP_LOGE("TCA9548A", "Invalid channel number: %d", channel);
    return ESP_ERR_INVALID_ARG;
  }

  // 2. 根据通道号生成正确的掩码
  uint8_t channel_mask = 1 << channel;

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (TCA9548A_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, channel_mask, true);
  i2c_master_stop(cmd);

  esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}

esp_err_t tca9548a_disable_all_channels(void) {
  // 向 TCA9548A 发送 0x00，关闭所有通道
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (TCA9548A_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, 0x00, true); // 写入 0x00
  i2c_master_stop(cmd);
  esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);
  return ret;
}