/**
 * @file main.c
 * @brief 系统入口，负责整体生命周期与子系统调度初始化。
 *
 * 依次拉起 UI 线程、后台网络线程、外设线程与语音唤醒任务，实现多任务并发。
 */
#include "aht20_sensor.h"
#include "app_event.h"
#include "audio_player.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_mic_input.h"
#include "lvgl_ui_task.h"
#include "net_mgr.h"
#include "peripherals_task.h"
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
  esp_sntp_setservername(1, "ntp.aliyun.com");
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

static void mic_test_task(void *pvParameters) {
  ESP_LOGI("MIC_TEST", "Starting microphone test...");
  while (1) {
    float rms = 0.0f;
    // 采集 200 毫秒的音频并计算 RMS 能量值（音量大小）
    esp_err_t err = i2s_mic_input_read_rms(&rms, 200);
    if (err == ESP_OK) {
      // 你可以通过对着麦克风吹气或说话，观察这个值是否明显变大
      ESP_LOGI("MIC_TEST", "Volume (RMS): %.2f", rms);
    } else {
      ESP_LOGE("MIC_TEST", "Mic read failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // 歇一小会儿
  }
}

/**
 * @brief 应用程序主入口
 *
 * 完成外设电源、子任务分发、WiFi连接等系统级初始化。
 */
void app_main(void) {

  wavesahre_rgb_lcd_bl_off();

  printf("PSRAM size: %d\n", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));

  /* 1. UI 线程初始化拉起 */
  app_ui_init();

  // 延迟点亮背光，避免白屏闪烁
  vTaskDelay(pdMS_TO_TICKS(100));
  wavesahre_rgb_lcd_bl_on();

  /* 2. 网络连接初始化 */
  ESP_LOGI(TAG, "Starting Wi-Fi...");
  net_mgr_init();
  esp_err_t ret = net_mgr_wait_connected(30000);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi failed, continuing with offline mode");
  }

  /* 3. 时间同步 */
  sntp_init_and_sync();

  /* 4. 外设与事件系统初始化（音频播放器 + 统一事件中心） */
  app_peripherals_init();

  // // 初始化音频播放器
  // esp_err_t ret_audio = audio_player_init();
  // if (ret_audio != ESP_OK) {
  //   ESP_LOGE(TAG, "audio_player_init failed: %s",
  //   esp_err_to_name(ret_audio));
  // }

  // // 初始化全局事件中心
  // esp_err_t ret_event = app_event_init();
  // if (ret_event != ESP_OK) {
  //   ESP_LOGE(TAG, "app_event_init failed: %s", esp_err_to_name(ret_event));
  // }

  // // 触发开机启动提示音事件
  // app_event_send(APP_EVENT_PLAY_STARTUP, 0);

  /* 5. 语音助手（独立任务，内部等待 UI 就绪后启动 ESP-SR） */
  // app_voice_assistant_init();

  /* 6. 临时麦克风测试任务 */
  // BaseType_t ret_task =
  //     xTaskCreate(mic_test_task, "mic_test", 4096, NULL, 5, NULL);
  // if (ret_task != pdPASS) {
  //   ESP_LOGE(TAG, "Failed to create mic_test_task! Memory might be full.");
  // }
}
