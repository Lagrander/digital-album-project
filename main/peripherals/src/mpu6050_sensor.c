#include "mpu6050_sensor.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "ui_main.h" // 引用 UI 方向设置接口
#include <stdio.h>
#include <stdlib.h>

#define MPU6050_I2C_NUM I2C_NUM_0
#define MPU6050_ADDR 0x68
#define MPU6050_PWR_MGMT_1 0x6B
#define MPU6050_ACCEL_XOUT_H 0x3B

// 引脚默认配置，兼容当前 LCD 总线
#define MPU6050_I2C_SDA_IO 8
#define MPU6050_I2C_SCL_IO 9

static const char *TAG = "MPU6050";
static bool mpu6050_initialized = false;

esp_err_t mpu6050_init(void) {
  if (mpu6050_initialized)
    return ESP_OK;

  // 安全初始化 I2C 总线，避免与其他驱动（如 LCD 背光）冲突
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = MPU6050_I2C_SDA_IO,
      .scl_io_num = MPU6050_I2C_SCL_IO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 400000,
  };
  esp_err_t err = i2c_param_config(MPU6050_I2C_NUM, &conf);
  if (err == ESP_OK) {
    err = i2c_driver_install(MPU6050_I2C_NUM, conf.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_FAIL && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
      return err;
    }
  }

  // 唤醒 MPU6050 (向 PWR_MGMT_1 写入 0x00)
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, MPU6050_PWR_MGMT_1, true);
  i2c_master_write_byte(cmd, 0x00, true);
  i2c_master_stop(cmd);
  err = i2c_master_cmd_begin(MPU6050_I2C_NUM, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "MPU6050 initialized successfully.");
    mpu6050_initialized = true;
  } else {
    ESP_LOGW(TAG, "Failed to wake up MPU6050. Check wiring.");
  }
  return err;
}

esp_err_t mpu6050_read_accel(int16_t *x, int16_t *y, int16_t *z) {
  if (!mpu6050_initialized || !x || !y || !z)
    return ESP_ERR_INVALID_STATE;

  uint8_t data[6];
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, MPU6050_ACCEL_XOUT_H, true);
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, (MPU6050_ADDR << 1) | I2C_MASTER_READ, true);
  i2c_master_read(cmd, data, 5, I2C_MASTER_ACK);
  i2c_master_read_byte(cmd, data + 5, I2C_MASTER_NACK);
  i2c_master_stop(cmd);
  esp_err_t err =
      i2c_master_cmd_begin(MPU6050_I2C_NUM, cmd, pdMS_TO_TICKS(100));
  i2c_cmd_link_delete(cmd);

  if (err == ESP_OK) {
    *x = (int16_t)((data[0] << 8) | data[1]);
    *y = (int16_t)((data[2] << 8) | data[3]);
    *z = (int16_t)((data[4] << 8) | data[5]);
  }
  return err;
}

void peripheral_mpu6050(void) {
  static int portrait_count = 0;
  static int landscape_count = 0;
  static bool current_is_portrait = true; // 假设初始为竖屏

  int16_t accel_x = 0, accel_y = 0, accel_z = 0;
  if (mpu6050_read_accel(&accel_x, &accel_y, &accel_z) == ESP_OK) {
    bool is_portrait = abs(accel_y) > abs(accel_x);

    if (is_portrait) {
      portrait_count++;
      landscape_count = 0;
    } else {
      landscape_count++;
      portrait_count = 0;
    }

    // 连续 2 次判定一致，且状态发生变化时才触发 UI 旋转
    if (portrait_count >= 2 && !current_is_portrait) {
      current_is_portrait = true;
      ESP_LOGI(TAG, "Orientation changed to Portrait.");
      ui_set_screen_rotation(true);
    } else if (landscape_count >= 2 && current_is_portrait) {
      current_is_portrait = false;
      ESP_LOGI(TAG, "Orientation changed to Landscape.");
      ui_set_screen_rotation(false);
    }
  }
}
