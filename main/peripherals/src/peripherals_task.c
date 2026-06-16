/**
 * @file peripherals_task.c
 * @brief 外围设备任务控制逻辑。
 *
 * 管理温湿度传感器与香薰雾化器的定期轮询与状态同步。
 */
#include "peripherals_task.h"
#include "aht20_sensor.h"
#include "driver/i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "he30.h"
#include "mpu6050_sensor.h"
#include "pcf8574_io.h"
#include "stream_player.h"
#include "tca9548a.h"
#include "voice_assistant.h"
#include "voice_io.h"
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static TaskHandle_t audio_task_handle = NULL;
static volatile bool audio_is_playing = false;
static bool s_audio_inited = false;

static int s_aroma_level[3] = {3, 3, 3};
static bool s_aroma_target[3] = {false, false, false};

void peripherals_set_aroma(int channel, bool on, int level) {
  if (channel >= 0 && channel < 3) {
    s_aroma_target[channel] = on;
    if (level >= 1 && level <= 3) {
      s_aroma_level[channel] = level;
    }
    ESP_LOGI("peripherals", "Aroma CH %d set to %s, level: %d", channel,
             on ? "ON" : "OFF", s_aroma_level[channel]);
  }
}

static bool s_humidity_blocked = false;

void peripherals_mist_level_up(void) {
  for (int i = 0; i < 3; i++) {
    if (s_aroma_target[i] && s_aroma_level[i] < 3) {
      s_aroma_level[i]++;
      ESP_LOGI("peripherals", "Mist CH %d level up → %d", i, s_aroma_level[i]);
    }
  }
}

void peripherals_mist_level_down(void) {
  for (int i = 0; i < 3; i++) {
    if (s_aroma_target[i] && s_aroma_level[i] > 1) {
      s_aroma_level[i]--;
      ESP_LOGI("peripherals", "Mist CH %d level down → %d", i, s_aroma_level[i]);
    }
  }
}

bool peripherals_is_humidity_blocked(void) {
  return s_humidity_blocked;
}

int peripherals_mist_channel_from_string(const char *ch_str) {
  if (!ch_str) return 0;
  if (strcmp(ch_str, "mint") == 0)    return 0;
  if (strcmp(ch_str, "jasmine") == 0) return 1;
  if (strcmp(ch_str, "rose") == 0)    return 2;
  return 0; // "none" or unknown → default channel 0
}

static void audio_test_task(void *arg) {
  // 播放测试 Beep 音前，强制对功放做冷启动物理复位以防抖和防止锁死
  voice_io_spk_force_reset();

  int16_t *beep = heap_caps_malloc(720, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!beep) {
    ESP_LOGE("audio_test", "Failed to allocate memory for beep");
    audio_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }
  for (int i = 0; i < 360; i++) {
    // 降低数字振幅，避免满载 30000 造成的极限爆音
    beep[i] = (int16_t)(10000.0f * sinf(2.0f * M_PI * 440.0f * i / 16000.0f));
  }

  ESP_LOGI("audio_test", "Audio task started, volume=%d%%, looping...",
           voice_io_get_spk_volume());
  uint32_t loops = 0;
  // 360 samples / 16000 Hz = 22.5ms，等待 22ms 匹配消费速率防止 buffer 爆满
  const TickType_t chunk_delay = pdMS_TO_TICKS(22);
  while (audio_is_playing) {
    esp_err_t err = voice_io_spk_play_stream((const uint8_t *)beep, 720);
    if (err == ESP_ERR_TIMEOUT) {
      // Ringbuf 消费不及，延长等待
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    } else if (err != ESP_OK) {
      ESP_LOGE("audio_test", "Play fail: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(100));
    } else {
      loops++;
      if (loops % 88 == 0) {
        ESP_LOGI("audio_test", "Playing audio... Elapsed: %lu seconds",
                 (unsigned long)(loops / 88));
      }
      // 匹配音频消费速率，防止 CPU 速度狂塞 ringbuf
      vTaskDelay(chunk_delay);
    }
  }

  ESP_LOGI("audio_test", "Audio task stopped, cleaning up...");
  voice_io_spk_stop();
  free(beep);
  audio_task_handle = NULL;
  vTaskDelete(NULL);
}

/**
 * @brief 外设任务主循环
 *
 * 负责初始化各种 I2C 传感器与 I/O 扩展器，并在死循环中周期性
 * 读取温湿度及同步雾化器状态。
 *
 * @param param 任务参数
 */
static void peripheral_aroma_process(int *tick_count) {
  *tick_count = (*tick_count + 1) % 10;

  // AHT20 湿度一票否决：湿度 > 80% 强制关闭所有香薰
  float hum = 0.0f;
  aht20_get_latest(NULL, &hum);
  bool was_blocked = s_humidity_blocked;
  s_humidity_blocked = (hum > 80.0f);

  for (int i = 0; i < 3; ++i) {
    bool physical_on = false;
    if (s_aroma_target[i] && !s_humidity_blocked) {
      int lv = s_aroma_level[i];
      if (lv >= 3) {
        physical_on = true; // 100% 开启
      } else if (lv == 2) {
        physical_on = (*tick_count < 5); // 50% 占空比 (1.5s开，1.5s停)
      } else if (lv == 1) {
        physical_on = (*tick_count < 2); // 20% 占空比 (0.6s开，2.4s停)
      }
    }
    he30_set_target(i, physical_on);
  }
  he30_sync();

  if (s_humidity_blocked && !was_blocked) {
    ESP_LOGW("peripherals", "Humidity %.1f%% > 80%% — mist blocked", hum);
  }
}

static void peripheral_privacy_check(int16_t ax, int16_t ay, int16_t az) {
  static bool s_privacy_active = false;
  if (az < -10000 && !s_privacy_active) {
    s_privacy_active = true;
    ESP_LOGW("peripherals", "Privacy mode triggered (device face down)!");
    voice_assistant_send_text("{\"event\":\"privacy_mode\"}");
  } else if (az > 10000 && s_privacy_active) {
    s_privacy_active = false;
    ESP_LOGI("peripherals", "Privacy mode exited (device face up).");
    voice_assistant_send_text("{\"event\":\"privacy_mode_exit\"}");
  }
}

static void peripheral_telemetry_report(int16_t ax, int16_t ay, int16_t az) {
  static int telemetry_count = 0;
  telemetry_count++;
  if (telemetry_count >= 15) {
    telemetry_count = 0;
    float temp = 0.0f, hum = 0.0f;
    aht20_get_latest(&temp, &hum);

    char telemetry_json[192];
    snprintf(telemetry_json, sizeof(telemetry_json),
             "{\"event\":\"telemetry\",\"temp\":%.1f,\"hum\":%.1f,\"ax\":%d,"
             "\"ay\":%d,\"az\":%d}",
             temp, hum, ax, ay, az);

    voice_assistant_send_text(telemetry_json);
  }
}

void app_peripherals(void *param) {
  (void)param;
  i2c_bus_mutex_init();

  if (i2c_sensor_aht20_init() != ESP_OK) {
    ESP_LOGE("peripherals", "AHT20 init failed!");
  }
  if (pcf8574_io_init() != ESP_OK) {
    ESP_LOGE("peripherals", "PCF8574 init failed!");
  }

  // 初始化 MPU6050
  if (mpu6050_init() != ESP_OK) {
    ESP_LOGE("peripherals", "MPU6050 init failed!");
  }

  // 初始化 3 个雾化器句柄，配置相关引脚
  he30_init_all(EXT_IO_PIN_0, EXT_IO_PIN_1, EXT_IO_PIN_2);

  static int tick_count = 0;

  while (1) {
    // 1. 加湿强度占空比计算与底层引脚控制 (间歇性喷雾)
    peripheral_aroma_process(&tick_count);
    
    // 2. 轮询传感器并更新内部数据缓存
    static int aht20_tick = 0;
    if (++aht20_tick >= 10) { // 300ms * 10 = 3s
      peripheral_aht20();
      aht20_tick = 0;
    }
    peripheral_mpu6050();

    // 3. 姿态传感器手势隐私模式判定与周期发送遥测数据
    int16_t ax = 0, ay = 0, az = 0;
    if (mpu6050_get_latest_accel(&ax, &ay, &az) == ESP_OK) {
      peripheral_privacy_check(ax, ay, az);
      peripheral_telemetry_report(ax, ay, az);
    }

    vTaskDelay(pdMS_TO_TICKS(300)); // 轮询周期 300ms
  }

  vTaskDelete(NULL);
}

static void uart_ctrl_task(void *arg) {
  static bool t_state[3] = {false, false, false};

  // 延时 3 秒，等 Wi-Fi 和屏幕初始化完毕
  vTaskDelay(pdMS_TO_TICKS(3000));
  
  // =========================================================================
  // 屏蔽开机自动播放 440Hz 蜂鸣声的测试代码，以防它干扰麦克风监听唤醒词！
  // =========================================================================
  /*
  ESP_LOGI("uart_ctrl", "Auto-starting audio test Beep (440Hz Sine)...");
  esp_err_t err = voice_io_spk_init(48000, 1, 16);
  if (err == ESP_OK) {
    audio_is_playing = true;
    BaseType_t ret = xTaskCreate(audio_test_task, "audio_test", 4096, NULL, 5,
                                 &audio_task_handle);
    if (ret == pdPASS) {
      ESP_LOGI("uart_ctrl", "audio_test_task auto-created successfully.");
    } else {
      ESP_LOGE("uart_ctrl", "Failed to auto-create audio_test_task: %d", ret);
    }
  } else {
    ESP_LOGE("uart_ctrl", "Audio init failed on startup");
  }
  */

  while (1) {
    int c = getchar();
    if (c != EOF) {
      switch (c) {
      case '1':
        t_state[0] = !t_state[0]; // Toggle
        he30_set_target(0, t_state[0]);
        ESP_LOGI("uart_ctrl", "Channel 1 %s", t_state[0] ? "ON" : "OFF");
        break;
      case '2':
        t_state[1] = !t_state[1]; // Toggle
        he30_set_target(1, t_state[1]);
        ESP_LOGI("uart_ctrl", "Channel 2 %s", t_state[1] ? "ON" : "OFF");
        break;
      case '3':
        t_state[2] = !t_state[2]; // Toggle
        he30_set_target(2, t_state[2]);
        ESP_LOGI("uart_ctrl", "Channel 3 %s", t_state[2] ? "ON" : "OFF");
        break;
      case '4': {
        // Toggle ALL
        bool new_state = !(t_state[0] && t_state[1] && t_state[2]);
        t_state[0] = t_state[1] = t_state[2] = new_state;
        he30_set_target(0, new_state);
        he30_set_target(1, new_state);
        he30_set_target(2, new_state);
        ESP_LOGI("uart_ctrl", "All Channels %s", new_state ? "ON" : "OFF");
        break;
      }
      case '5': {
        if (audio_task_handle != NULL) {
          ESP_LOGI("uart_ctrl", "Stopping audio test...");
          audio_is_playing = false;
          // 等待任务自行退出（最多 500ms），否则强制删除
          for (int i = 0; i < 10 && audio_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
          }
          if (audio_task_handle != NULL) {
            ESP_LOGW("uart_ctrl", "audio_test_task stuck after 500ms, force deleting");
            vTaskDelete(audio_task_handle);
            audio_task_handle = NULL;
            voice_io_spk_stop();
            ESP_LOGI("uart_ctrl", "Audio test force-stopped.");
          } else {
            ESP_LOGI("uart_ctrl", "Audio test stopped cleanly.");
          }
        } else {
          ESP_LOGI("uart_ctrl", "Starting audio test (440Hz Beep)...");
          if (!s_audio_inited) {
            esp_err_t err = voice_io_spk_init(48000, 1, 16);
            if (err == ESP_OK) {
              s_audio_inited = true;
            } else {
              ESP_LOGE("uart_ctrl", "Audio init failed, cannot play");
              break;
            }
          }
          // 强制将测试音量设定为 10%
          voice_io_set_spk_volume(10);
          audio_is_playing = true;
          BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
              audio_test_task, "audio_test", 6144, NULL, 5, &audio_task_handle,
              tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (ret == pdPASS) {
            ESP_LOGI("uart_ctrl", "audio_test_task created successfully.");
          } else {
            ESP_LOGE("uart_ctrl", "Failed to create audio_test_task: %d", ret);
          }
        }
        break;
      }
      case '6': {
        static bool stream_is_playing = false;
        if (stream_is_playing) {
          ESP_LOGI("uart_ctrl", "Stopping MP3 Stream...");
          stream_player_stop();
          stream_is_playing = false;
        } else {
          char url[128];
          snprintf(url, sizeof(url), "%s/music/test.mp3", CONFIG_SERVER_URL);
          ESP_LOGI("uart_ctrl", "Starting MP3 Stream (%s)...", url);
          if (!s_audio_inited) {
            esp_err_t err = voice_io_spk_init(48000, 1, 16);
            if (err == ESP_OK) {
              s_audio_inited = true;
            } else {
              ESP_LOGE("uart_ctrl", "Audio init failed, cannot play");
              break;
            }
          }
          if (voice_io_get_spk_volume() < 50) {
            voice_io_set_spk_volume(50);
          }
          stream_is_playing = true;
          stream_player_play_url(url);
        }
        break;
      }
      case '7': {
        ESP_LOGI("uart_ctrl", "Forcing voice assistant wakeup (simulated by key '7')...");
        va_force_wake();
        break;
      }
      case ' ':
        t_state[0] = t_state[1] = t_state[2] = false;
        he30_set_target(0, false);
        he30_set_target(1, false);
        he30_set_target(2, false);
        ESP_LOGI("uart_ctrl", "All Channels OFF");
        break;
      case '+':
      case '=': {
        uint8_t vol = voice_io_get_spk_volume();
        if (vol <= 90)
          vol += 10;
        else
          vol = 100;
        voice_io_set_spk_volume(vol);
        break;
      }
      case '-':
      case '_': {
        uint8_t vol = voice_io_get_spk_volume();
        if (vol >= 10)
          vol -= 10;
        else
          vol = 0;
        voice_io_set_spk_volume(vol);
        break;
      }
      default:
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/**
 * @brief 初始化外设任务
 *
 * 静态创建并拉起外设轮询任务。
 */
void app_peripherals_init(void) {
  static StaticTask_t task_tcb;
  static StackType_t task_stack[1024 * 4];

  TaskHandle_t task_peripherals =
      xTaskCreateStaticPinnedToCore(app_peripherals, "peripherals_task", 1024 * 4, NULL, 5,
                        task_stack, /* 数组 */
                        &task_tcb,  /*  TCB */
                        0           /* Core 0 */
      );

  if (task_peripherals == NULL) {
    printf("Failed to create peripherals task\n");
  }

  // 创建串口控制任务 (固定在 Core 0，不抢占语音)
  xTaskCreatePinnedToCore(uart_ctrl_task, "uart_ctrl_task", 4096, NULL, 1, NULL, 0);
}

/* ── 雾化器控制接口（线程安全）────────────────────────── */

/**
 * @brief 开启全部香薰/雾化通道
 */
void peripherals_mist_on(void) {
  for (int i = 0; i < 3; i++) {
    peripherals_set_aroma(i, true, 3);
  }
  ESP_LOGI("peripherals", "Voice → Mist ON (all 3 channels)");
}

/**
 * @brief 关闭全部香薰/雾化通道
 */
void peripherals_mist_off(void) {
  for (int i = 0; i < 3; i++) {
    peripherals_set_aroma(i, false, 3);
  }
  ESP_LOGI("peripherals", "Voice → Mist OFF (all 3 channels)");
}

/**
 * @brief 查询是否有任意一路雾化器处于开启状态
 *
 * @return true 至少有一路开启
 * @return false 全部关闭
 */
bool peripherals_mist_is_on(void) {
  for (int i = 0; i < 3; i++) {
    if (s_aroma_target[i])
      return true;
  }
  return false;
}