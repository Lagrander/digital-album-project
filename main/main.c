/**
 * @file main.c
 * @brief 系统入口，负责整体生命周期与子系统调度初始化。
 *
 * 依次拉起 UI 线程、后台网络线程、外设线程与语音唤醒任务，实现多任务并发。
 */
#include "aht20_sensor.h"
#include "app_event.h"
#include "audio_player.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_mic_input.h"
#include "lvgl_ui_task.h"
#include "net_mgr.h"
#include "peripherals_task.h"
#include "voice_assistant.h"
#include "voice_assistant_task.h"
#include "waveshare_rgb_lcd_port.h"
#include <stdio.h>

static const char *TAG = "main";

/* ── SNTP ──────────────────────────────────────────────────── */

/**
 * @brief 初始化 SNTP 服务并同步网络时间
 *
 * 采用轮询模式向配置的 NTP 服务器请求授时，并在同步成功后更新系统本地时区。
 */
static void sntp_init_and_sync(void) {
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  esp_sntp_setservername(1, "time.google.com");
  esp_sntp_setservername(2, "time.nist.gov");
  esp_sntp_init();

  // 设置本地时区为东八区（北京时间 CST-8）
  setenv("TZ", "CST-8", 1);
  tzset();

  int retry = 0;
  while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retry < 30) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    retry++;
  }
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    ESP_LOGI(TAG, "SNTP time synced");
  } else {
    ESP_LOGW(TAG, "SNTP sync timeout, continuing without accurate time");
  }
}

/* ── main ──────────────────────────────────────────────────── */

/**
 * @brief 应用程序主入口
 *
 * 完成外设电源、子任务分发、WiFi连接等系统级初始化。
 */
void app_main(void) {

  wavesahre_rgb_lcd_bl_off();

  ESP_LOGI(TAG, "PSRAM Total: %d bytes",
           heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  ESP_LOGI(TAG, "PSRAM Free before models: %d bytes, Largest block: %d bytes",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  /* 0. 提前预加载语音模型，防止后续内存碎片化导致无法分配 连续内存 */
  va_preload_models(0);

  ESP_LOGI(TAG, "PSRAM Free after models: %d bytes, Largest block: %d bytes",
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  /* 1. 网络连接初始化 */
  ESP_LOGI(TAG, "Starting Wi-Fi...");
  net_mgr_init();

  /* 2. UI 线程初始化拉起 */
  app_ui_init();

  vTaskDelay(pdMS_TO_TICKS(100));
  wavesahre_rgb_lcd_bl_on();

  esp_err_t ret = net_mgr_wait_connected(3000); // 30000
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi failed, continuing with offline mode");
  }

  /* 3. 时间同步 */
  sntp_init_and_sync();
  /* 4. 外设与事件系统初始化（音频播放器 + 统一事件中心） */
  app_peripherals_init();

  // 初始化全局事件中心
  esp_err_t ret_event = app_event_init();
  if (ret_event != ESP_OK) {
    ESP_LOGE(TAG, "app_event_init failed: %s", esp_err_to_name(ret_event));
  }

  /* 5. 语音助手（独立任务，内部等待 UI 就绪后启动 ESP-SR） */
  app_voice_assistant_init();
}