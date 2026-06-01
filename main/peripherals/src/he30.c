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

he30_handle_t he30_init(ext_io_pin_t pin, bool active_low)
{
    if (pcf8574_mutex == NULL) {
        pcf8574_mutex = xSemaphoreCreateMutex();
    }

    he30_handle_t handle = {
        .pin = pin,
        .active_low = active_low
    };

    // 初始化时先关闭雾化器
    he30_off(handle);
    ESP_LOGI(TAG, "HE-30 initialized on pin %d, active_low=%d", pin, active_low);
    return handle;
}

esp_err_t he30_on(he30_handle_t handle)
{
    if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
        uint32_t level = handle.active_low ? 0 : 1;
        esp_err_t ret = pcf8574_write_pin(handle.pin, level);
        xSemaphoreGive(pcf8574_mutex);
        return ret;
    }
    return ESP_FAIL;
}

esp_err_t he30_off(he30_handle_t handle)
{
    if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
        uint32_t level = handle.active_low ? 1 : 0;
        esp_err_t ret = pcf8574_write_pin(handle.pin, level);
        xSemaphoreGive(pcf8574_mutex);
        return ret;
    }
    return ESP_FAIL;
}

// 初始化 & 状态管理
void he30_init_all(ext_io_pin_t pin0, ext_io_pin_t pin1, ext_io_pin_t pin2)
{
    if (is_initialized) return;

    he30_handles[0].handle = he30_init(pin0, true);
    he30_handles[1].handle = he30_init(pin1, true);
    he30_handles[2].handle = he30_init(pin2, true);

    // 初始状态全部为关闭
    for (int i = 0; i < 3; i++) {
        he30_handles[i].is_on = false;
        target_state[i] = false;
    }
    is_initialized = true;
    ESP_LOGI(TAG, "All 3 HE-30 initialized on pins %d, %d, %d", pin0, pin1, pin2);
}

bool he30_get_state(int index)
{
    if (index < 0 || index > 2) return false;
    return he30_handles[index].is_on;
}

// 仅在硬件操作成功后才更新 is_on
esp_err_t he30_set_state(int index, bool on)
{
    if (index < 0 || index > 2) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    if (on) {
        ret = he30_on(he30_handles[index].handle);
    } else {
        ret = he30_off(he30_handles[index].handle);
    }

    if (ret == ESP_OK) {
        he30_handles[index].is_on = on;
    } else {
        ESP_LOGE(TAG, "Failed to set state for module %d to %d: %s", index, on, esp_err_to_name(ret));
    }
    return ret;
}

// 其他task可修改状态 & 循环同步函数
void he30_set_target(int index, bool target)
{
    if (index < 0 || index > 2) return;
    if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
        target_state[index] = target;
        xSemaphoreGive(pcf8574_mutex);
        ESP_LOGI(TAG, "Target for module %d set to %d", index, target);
    }
}

void he30_sync(void)
{
    if (!is_initialized) {
        ESP_LOGE(TAG, "HE-30 not initialized!");
        return;
    }

    if (pcf8574_mutex && xSemaphoreTake(pcf8574_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < 3; i++) {
            // 如果目标状态与当前实际状态不一致，则执行操作
            if (target_state[i] != he30_handles[i].is_on) {
                // 尝试切换硬件，并仅在成功时更新 is_on
                esp_err_t ret = (target_state[i]) ? he30_on(he30_handles[i].handle) : he30_off(he30_handles[i].handle);
                if (ret == ESP_OK) {
                    he30_handles[i].is_on = target_state[i];
                    ESP_LOGI(TAG, "Module %d synced to target %d", i, target_state[i]);
                } else {
                    ESP_LOGE(TAG, "Sync failed for module %d: %s", i, esp_err_to_name(ret));
                }
            }
        }
        xSemaphoreGive(pcf8574_mutex);
    }
}