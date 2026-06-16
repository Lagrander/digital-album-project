/**
 * @file pcf8574_io.c
 * @brief I2C 低速 GPIO 扩展与多核并发锁机制引擎
 *
 * @architecture
 * 本文件对 PCF8574 芯片进行抽象，用于缓解 ESP32 核心 GPIO 紧张问题。
 * 1. 并发安全机制：内置 FreeRTOS 互斥锁 s_mutex，彻底解决 Core 0 的 UI 采样任务与 Core 1 的香薰控制任务并发竞争 I2C 总线造成的宕机问题。
 * 2. 物理防抖：在上电瞬间写入 0xFF (释放引脚为弱上拉)，防止 HE30 或其他大负载开机误导通。
 */
#include "pcf8574_io.h"
#include "tca9548a.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "PCF8574_IO";

#define PCF8574_PIN_COUNT 8
// PCF8574 写入 1 会释放引脚并使能弱上拉。作为通用驱动的默认状态，这在物理上
// 比强制写 0 安全得多，可以避免上电瞬间低电平有效的外设（如 Active-Low
// 香薰）意外动作。
#define PCF8574_SAFE_PORT_STATE 0xFF

#ifndef PCF8574_I2C_PORT
#define PCF8574_I2C_PORT I2C_NUM_0
#endif

#ifndef PCF8574_I2C_SDA_GPIO
#ifdef CONFIG_I2C_MASTER_SDA
#define PCF8574_I2C_SDA_GPIO CONFIG_I2C_MASTER_SDA
#else
#define PCF8574_I2C_SDA_GPIO 8
#endif
#endif

#ifndef PCF8574_I2C_SCL_GPIO
#ifdef CONFIG_I2C_MASTER_SCL
#define PCF8574_I2C_SCL_GPIO CONFIG_I2C_MASTER_SCL
#else
#define PCF8574_I2C_SCL_GPIO 9
#endif
#endif

#ifndef PCF8574_I2C_ADDR
#define PCF8574_I2C_ADDR 0x27
#endif

static bool s_initialized;
static uint8_t s_port_state = PCF8574_SAFE_PORT_STATE;

i2c_dev_t pcf8574_dev;

static bool pcf8574_pin_is_valid(uint8_t pin) {
  return pin < PCF8574_PIN_COUNT;
}

static esp_err_t pcf8574_lock(void) {
  i2c_bus_lock();
  return ESP_OK;
}

static void pcf8574_unlock(void) { i2c_bus_unlock(); }

/**
 * @brief 初始化 PCF8574 I/O 扩展模块
 *
 * 配置 I2C 总线并初始化设备描述符。初始化时默认读取一次芯片管脚电平，
 * 若读取失败则强制写入默认的安全高电平状态。
 *
 * @return esp_err_t 返回 ESP_OK 则初始化成功
 */
esp_err_t pcf8574_init(void) {
  esp_err_t ret = pcf8574_lock();
  if (ret != ESP_OK) {
    return ret;
  }

  if (s_initialized) {
    pcf8574_unlock();
    return ESP_OK;
  }

  // 调用 esp-idf-lib/i2cdev 的系统总线初装，避免重复调用 i2c_driver_install
  // 发生端口冲突
  ret = i2cdev_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2C device subsystem init failed: %s", esp_err_to_name(ret));
    pcf8574_unlock();
    return ret;
  }

  // 强制只探测 0x27
  const uint8_t candidate_addrs[] = {0x27};
  bool found = false;
  uint8_t actual_addr = 0;

  for (int i = 0; i < sizeof(candidate_addrs); i++) {
    memset(&pcf8574_dev, 0, sizeof(pcf8574_dev));
    ret = pcf8574_init_desc(&pcf8574_dev, candidate_addrs[i], PCF8574_I2C_PORT,
                            PCF8574_I2C_SDA_GPIO, PCF8574_I2C_SCL_GPIO);

    if (ret == ESP_OK) {
      pcf8574_dev.cfg.sda_pullup_en = true;
      pcf8574_dev.cfg.scl_pullup_en = true;

      // 尝试读取一次端口状态
      ret = pcf8574_port_read(&pcf8574_dev, &s_port_state);
      if (ret == ESP_OK) {
        found = true;
        actual_addr = candidate_addrs[i];
        ESP_LOGI(TAG, "Successfully probed PCF8574 at address 0x%02x",
                 actual_addr);
        break;
      } else {
        // 如果读取失败，清理描述符，尝试下一个
        i2c_dev_delete_mutex(&pcf8574_dev);
      }
    }
  }

  if (!found) {
    ESP_LOGE(TAG, "Failed to find PCF8574 on any candidate address. Hardware "
                  "or wiring issue.");
    pcf8574_unlock();
    return ESP_FAIL;
  }

  s_initialized = true;

  // 【方案一：强制初始化】在发现芯片后，立刻强制写入 0x00，确保所有引脚拉低
  esp_err_t force_ret = pcf8574_port_write(&pcf8574_dev, 0x00);
  if (force_ret == ESP_OK) {
    s_port_state = 0x00;
    ESP_LOGI(TAG, "Forced all PCF8574 pins to LOW (0x00) on init.");
  } else {
    ESP_LOGW(TAG, "Failed to force pins to LOW: %s",
             esp_err_to_name(force_ret));
  }

  ESP_LOGI(TAG, "Initialized addr=0x%02x SDA=%d SCL=%d state=0x%02x",
           actual_addr, PCF8574_I2C_SDA_GPIO, PCF8574_I2C_SCL_GPIO,
           s_port_state);
  pcf8574_unlock();
  return ESP_OK;
}

/**
 * @brief I/O 初始化包装函数
 *
 * @return esp_err_t
 */
esp_err_t pcf8574_io_init(void) { return pcf8574_init(); }

/**
 * @brief 写单个扩展引脚电平
 *
 * @param pin   要写入的扩展引脚枚举 (ext_io_pin_t)
 * @param level 目标电平 (1 为高，0 为低)
 * @return esp_err_t 返回 ESP_OK 写入成功
 */
esp_err_t pcf8574_write_pin(ext_io_pin_t pin, uint32_t level) {
  if (!pcf8574_pin_is_valid((uint8_t)pin)) {
    ESP_LOGE(TAG, "Invalid pin: %u", pin);
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = pcf8574_init();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = pcf8574_lock();
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t next_state =
      level ? (s_port_state | BIT(pin)) : (s_port_state & (uint8_t)~BIT(pin));
  ret = pcf8574_port_write(&pcf8574_dev, next_state);
  if (ret == ESP_OK) {
    s_port_state = next_state;
  } else {
    ESP_LOGE(TAG, "Write pin %u failed: %s", pin, esp_err_to_name(ret));
  }

  pcf8574_unlock();
  return ret;
}

/**
 * @brief 读单个扩展引脚电平
 *
 * @param pin   要读取的扩展引脚枚举 (ext_io_pin_t)
 * @param level 返回读取到的电平值 (1 为高，0 为低)
 * @return esp_err_t 返回 ESP_OK 读取成功
 */
esp_err_t pcf8574_read_pin(ext_io_pin_t pin, uint32_t *level) {
  if (!pcf8574_pin_is_valid((uint8_t)pin) || level == NULL) {
    ESP_LOGE(TAG, "Invalid read pin args: pin=%u level=%p", pin, level);
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t value = 0;
  esp_err_t ret = pcf8574_read_port(&value);
  if (ret == ESP_OK) {
    *level = (value & BIT(pin)) ? 1 : 0;
  }

  return ret;
}

/**
 * @brief 写整个 8 位端口电平
 *
 * @param value 要写入的 8 位整型值
 * @return esp_err_t
 */
esp_err_t pcf8574_write_port(uint8_t value) {
  esp_err_t ret = pcf8574_init();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = pcf8574_lock();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = pcf8574_port_write(&pcf8574_dev, value);
  if (ret == ESP_OK) {
    s_port_state = value;
    ESP_LOGI(TAG, "Port write state=0x%02x", s_port_state);
  } else {
    ESP_LOGE(TAG, "Port write failed: %s", esp_err_to_name(ret));
  }

  pcf8574_unlock();
  return ret;
}

/**
 * @brief 读整个 8 位端口电平
 *
 * @param value 返回读取到的 8 位整型值
 * @return esp_err_t
 */
esp_err_t pcf8574_read_port(uint8_t *value) {
  if (value == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = pcf8574_init();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = pcf8574_lock();
  if (ret != ESP_OK) {
    return ret;
  }

  ret = pcf8574_port_read(&pcf8574_dev, value);
  if (ret == ESP_OK) {
    s_port_state = *value;
  } else {
    ESP_LOGE(TAG, "Port read failed: %s", esp_err_to_name(ret));
  }

  pcf8574_unlock();
  return ret;
}

uint8_t pcf8574_get_cached_port(void) { return s_port_state; }