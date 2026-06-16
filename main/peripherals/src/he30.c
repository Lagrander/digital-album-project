/**
 * @file he30.c
 * @brief 三通道香薰电磁阀隔离驱动器
 *
 * @architecture
 * 封装 PCF8574 IO，实现微安级弱电平控制，以隔离驱动 HE30 大电流工作板。
 */
#include "he30.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "HE30";
static SemaphoreHandle_t pcf8574_mutex = NULL;

// 核心状态存储
static he30_control_t he30_handles[3];
static bool target_state[3] = {false, false, false};
static bool is_initialized = false;

he30_handle_t he30_init(ext_io_pin_t pin, bool active_low) {
  if (pcf8574_mutex == NULL) {
    pcf8574_mutex = xSemaphoreCreateMutex();
  }

  he30_handle_t handle = {.pin = pin, .active_low = active_low};

  // 初始化时先关闭雾化器
  he30_off(handle);
  ESP_LOGI(TAG, "HE-30 initialized on pin %d, active_low=%d", pin, active_low);
  return handle;
}

esp_err_t he30_on(he30_handle_t handle) {
  ESP_LOGD(TAG, "he30_on: taking pcf8574_mutex");
  if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
    ESP_LOGD(TAG, "he30_on: pcf8574_mutex TAKEN. Calling pcf8574_write_pin");
    uint32_t level = handle.active_low ? 0 : 1;
    esp_err_t ret = pcf8574_write_pin(handle.pin, level);
    ESP_LOGD(TAG, "he30_on: pcf8574_write_pin returned %d. Giving pcf8574_mutex", ret);
    xSemaphoreGive(pcf8574_mutex);
    return ret;
  }
  ESP_LOGE(TAG, "he30_on: Failed to take pcf8574_mutex");
  return ESP_FAIL;
}

esp_err_t he30_off(he30_handle_t handle) {
  if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
    uint32_t level = handle.active_low ? 1 : 0;
    esp_err_t ret = pcf8574_write_pin(handle.pin, level);
    xSemaphoreGive(pcf8574_mutex);
    return ret;
  }
  return ESP_FAIL;
}

// 初始化 & 状态管理
void he30_init_all(ext_io_pin_t pin0, ext_io_pin_t pin1, ext_io_pin_t pin2) {
  if (is_initialized)
    return;

  /* 高电平开启，低电平关闭 */
  he30_handles[0].handle = he30_init(pin0, false);
  he30_handles[1].handle = he30_init(pin1, false);
  he30_handles[2].handle = he30_init(pin2, false);

  // 初始状态全部为关闭
  for (int i = 0; i < 3; i++) {
    he30_handles[i].is_on = false;
    target_state[i] = false;
  }
  is_initialized = true;
  ESP_LOGI(TAG, "All 3 HE-30 initialized on pins %d, %d, %d", pin0, pin1, pin2);
}

bool he30_get_state(int index) {
  if (index < 0 || index > 2)
    return false;
  return he30_handles[index].is_on;
}

// 仅在硬件操作成功后才更新 is_on
esp_err_t he30_set_state(int index, bool on) {
  if (index < 0 || index > 2)
    return ESP_ERR_INVALID_ARG;

  esp_err_t ret = ESP_OK;
  if (on) {
    ret = he30_on(he30_handles[index].handle);
  } else {
    ret = he30_off(he30_handles[index].handle);
  }

  if (ret == ESP_OK) {
    he30_handles[index].is_on = on;
  } else {
    ESP_LOGE(TAG, "Failed to set state for module %d to %d: %s", index, on,
             esp_err_to_name(ret));
  }
  return ret;
}

// 其他task可修改状态 & 循环同步函数
void he30_set_target(int index, bool target) {
  if (index < 0 || index > 2)
    return;
  if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
    target_state[index] = target;
    xSemaphoreGive(pcf8574_mutex);
    ESP_LOGD(TAG, "Target for module %d set to %d", index, target);
  }
}

void he30_sync(void) {
  if (!is_initialized) {
    return;
  }

  for (int i = 0; i < 3; i++) {
    bool target = false;
    
    if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
      target = target_state[i];
      xSemaphoreGive(pcf8574_mutex);
    } else {
      continue;
    }

    if (target != he30_handles[i].is_on) {
      ESP_LOGD(TAG, "he30_sync: About to call he30_on/off for module %d, target=%d", i, target);
      esp_err_t ret = (target) ? he30_on(he30_handles[i].handle)
                               : he30_off(he30_handles[i].handle);
      ESP_LOGD(TAG, "he30_sync: Returned from he30_on/off with code %d", ret);
      if (ret == ESP_OK) {
        he30_handles[i].is_on = target;
        ESP_LOGD(TAG, "Module %d synced to target %d", i, target);
      } else {
        ESP_LOGE(TAG, "Sync failed for module %d: %s", i,
                 esp_err_to_name(ret));
      }
    }
  }
}