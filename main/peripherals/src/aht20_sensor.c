/**
 * @file aht20_sensor.c
 * @brief 提供 AHT20 温湿度传感器的初始化、读取与缓存接口。
 *
 * 封装底层的 I2C 交互逻辑，并对外提供非阻塞的数据读取方法。
 */
/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "aht20_sensor.h"
#include "aht20.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tca9548a.h"

static const char *TAG = "aht20";

static i2c_bus_handle_t i2c_bus;
static aht20_dev_handle_t aht20 = NULL;
static bool aht20_initialized = false;

// Cached latest readings
static float g_latest_temp = 0.0f;
static float g_latest_hum = 0.0f;
static bool g_data_valid = false;

/**
 * @brief 初始化 AHT20 传感器和其所在的 I2C 总线
 *
 * 创建 I2C 主机总线实例，随后向其挂载 AHT20 设备句柄。
 *
 * @return esp_err_t 返回 ESP_OK 则初始化成功
 */
esp_err_t i2c_sensor_aht20_init(void) {
  if (aht20_initialized) {
    return ESP_OK;
  }

  const i2c_config_t i2c_bus_conf = {.mode = I2C_MODE_MASTER,
                                     .sda_io_num = I2C_MASTER_SDA_IO,
                                     .sda_pullup_en = GPIO_PULLUP_ENABLE,
                                     .scl_io_num = I2C_MASTER_SCL_IO,
                                     .scl_pullup_en = GPIO_PULLUP_ENABLE,
                                     .master.clk_speed = I2C_MASTER_FREQ_HZ};
  i2c_bus = i2c_bus_create(I2C_MASTER_NUM, &i2c_bus_conf);

  if (i2c_bus == NULL) {
    ESP_LOGE(TAG, "i2c_bus create returned NULL");
    return ESP_ERR_INVALID_ARG;
  }

  aht20_i2c_config_t i2c_conf = {
      .bus_inst = i2c_bus,
      .i2c_addr = AHT20_ADDRRES_0,
  };

  i2c_bus_lock();
  esp_err_t ret = tca9548a_select_channel(2);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Init: Failed to select channel 2: %s", esp_err_to_name(ret));
    i2c_bus_unlock();
    return ret;
  }

  ret = aht20_new_sensor(&i2c_conf, &aht20);

  if (ret == ESP_OK && aht20 != NULL) {
    // AHT20 必须在上电后发送 0xBE 初始化指令，否则后续测量数据会是乱码并导致
    // CRC 校验失败
    uint8_t init_cmd[2] = {0x08, 0x00};
    i2c_bus_write_bytes((i2c_bus_device_handle_t)aht20, 0xBE, 2, init_cmd);
    vTaskDelay(pdMS_TO_TICKS(20)); // 等待初始化完成
  }

  tca9548a_disable_all_channels();
  i2c_bus_unlock();

  if (ret != ESP_OK || aht20 == NULL) {
    ESP_LOGE(TAG, "AHT20 create failed: %s", esp_err_to_name(ret));
    return (ret == ESP_OK) ? ESP_FAIL : ret;
  }

  aht20_initialized = true;
  return ESP_OK;
}

/**
 * @brief 获取缓存的最新温湿度数据
 *
 * 非阻塞读取，若传感器暂无有效数据则返回失败。
 *
 * @param temp 输出参数，返回最近读取的温度（摄氏度）
 * @param hum  输出参数，返回最近读取的相对湿度（百分比）
 * @return esp_err_t 返回 ESP_OK 获取成功
 */
esp_err_t aht20_get_latest(float *temp, float *hum) {
  if (!g_data_valid || !temp || !hum) {
    return ESP_FAIL;
  }
  *temp = g_latest_temp;
  *hum = g_latest_hum;
  return ESP_OK;
}

/**
 * @brief 内部方法：主动向 AHT20 传感器发起一次采样读取
 *
 * 阻塞发起 I2C 读取，成功后更新内部温湿度缓存变量。
 */
void peripheral_aht20() {
  esp_err_t ret = ESP_FAIL;
  i2c_bus_lock();

  uint32_t temperature_raw;
  uint32_t humidity_raw;
  float temperature;
  float humidity;

  ret = tca9548a_select_channel(2);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to select channel: %s", esp_err_to_name(ret));
    goto cleanup; // error 跳转
  }

  ret = aht20_read_temperature_humidity(aht20, &temperature_raw, &temperature,
                                        &humidity_raw, &humidity);

  if (ret == ESP_OK) {
    g_latest_temp = temperature;
    g_latest_hum = humidity;
    g_data_valid = true;

    // 每读取 6 次才打印 1 次（大约每 4 秒打印一次），降低刷屏频率
    static int print_counter = 0;
    if (++print_counter >= 10) {
      ESP_LOGI(TAG, "humidity: %2.2f %%  temperature: %2.2f degC", humidity,
               temperature);
      print_counter = 0;
    }
  } else {
    ESP_LOGE(TAG, "Failed to read: %s", esp_err_to_name(ret));
  }

cleanup:
  tca9548a_disable_all_channels(); // 读完后关闭总线
  i2c_bus_unlock();
}

/**
 * @brief FreeRTOS 任务封装：执行一次采样并在结束后销毁自身
 *
 * @param arg 任务参数（未使用）
 */
void aht20_process(void *arg) {
  peripheral_aht20();
  vTaskDelete(NULL);
}
