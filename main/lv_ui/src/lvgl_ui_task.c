/**
 * @file lvgl_ui_task.c
 * @brief LVGL UI 任务主循环与多核任务调度协调模块。
 *
 * 涉及 FreeRTOS 的亲和性调度设计：后台网络与指令轮询运行在 Core 0，
 * 避免阻塞运行在 Core 1 的 UI 渲染主任务。使用队列和互斥锁实现安全的数据交接。
 */

#include "../../voice_assistant/include/voice_assistant.h"
#include "aht20_sensor.h"
#include "aroma_ctrl.h"
#include "esp_system.h"
#include "net_mgr.h"
#include "photo_client.h"
#include "ui_main.h"
#include "waveshare_rgb_lcd_port.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <string.h>
#include "../../voice_assistant/include/stream_player.h"
#include "../../peripherals/include/peripherals_task.h"

void ui_resume_playlist(void);
static bool g_sleep_mode = false;
static volatile bool g_rotate_requested = false;

void ui_voice_next_photo(void);

static const char *TAG = "lv_ui";

// 异步上传队列缓存设计（Core 0 写入，Core 1 消费）
typedef struct {
  uint8_t *buf;
  size_t len;
  char message[128];
  char uploader[64];
} pending_upload_t;

static pending_upload_t g_pending_upload;
static volatile bool g_has_pending_upload = false;
static SemaphoreHandle_t g_pending_mutex = NULL;

// 照片缓冲区（PSRAM）
static uint8_t *g_photo_buf = NULL;
// 备份照片缓冲区（用以在上传照片渐变退场期间，临时展示旧图而不至于野指针崩溃）
static uint8_t *g_old_photo_buf = NULL;

/**
 * @brief 释放过渡用旧图片缓冲区
 *
 * 由 ui_main.c 中的动画结束回调触发，安全回收旧图片内存。
 * @return void
 */
void lvgl_ui_release_old_photo(void) {
  if (g_old_photo_buf) {
    photo_client_free_buf(g_old_photo_buf);
    g_old_photo_buf = NULL;
    ESP_LOGI(TAG, "Old photo buffer released after crossfade");
  }
}
photo_metadata_t g_photos[MAX_PHOTOS_PER_DAY];
int g_photo_count = 0;
int g_current_idx = 0;

/* ── 核心逻辑 ────────────────────────────────────────────── */
/**
 * @brief 下载并显示指定索引的照片
 *
 * @param idx 照片在列表中的索引
 * @return void
 */
void fetch_and_display_photo(int idx) {
  if (idx < 0 || idx >= g_photo_count)
    return;

  size_t len = 0;
  uint8_t *new_buf = NULL;
  esp_err_t ret =
      photo_client_download_rgb565(g_photos[idx].id, &new_buf, &len);

  if (ret == ESP_OK && new_buf) {
    // UI 更新需要 LVGL 锁
    if (lvgl_port_lock(-1)) {
      if (g_photo_buf) {
        photo_client_free_buf(g_photo_buf);
      }
      g_photo_buf = new_buf;
      ui_set_photo_data(g_photo_buf, len, g_photos[idx].caption,
                        g_photos[idx].city, g_photos[idx].date);
      lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "Photo %d displayed: %s", idx, g_photos[idx].id);
  } else {
    ESP_LOGE(TAG, "Failed to download photo %d", idx);
    if (lvgl_port_lock(-1)) {
      if (g_photo_buf) {
        photo_client_free_buf(g_photo_buf);
        g_photo_buf = NULL;
      }
      ui_show_placeholder();
      lvgl_port_unlock();
    }
  }
}

/**
 * @brief 后台网络轮询任务 (Core 0)
 *
 * 定期发送心跳上报状态、拉取遥控指令，以及检查是否有新上传照片。
 *
 * @param param 任务参数
 * @return void
 */
static void upload_check_task(void *param) {
  (void)param;
  ESP_LOGI(TAG, "Unified Network Daemon started (Core 0)");

  ESP_LOGI(TAG, "Waiting for network connection in background task...");
  int wait_net = 0;
  while (!net_mgr_is_connected() && wait_net < 150) {
    vTaskDelay(pdMS_TO_TICKS(100));
    wait_net++;
  }

  if (net_mgr_is_connected()) {
    ESP_LOGI(TAG, "Fetching today's photos...");
    photo_client_fetch_today(g_photos, &g_photo_count);
    ESP_LOGI(TAG, "Photo count: %d", g_photo_count);
    if (g_photo_count > 0) {
      fetch_and_display_photo(0);
    } else {
      if (lvgl_port_lock(-1)) {
        ui_show_placeholder();
        lvgl_port_unlock();
      }
    }
  } else {
    ESP_LOGW(TAG, "Not connected after timeout, skipping photo fetch");
    if (lvgl_port_lock(-1)) {
      ui_show_placeholder();
      lvgl_port_unlock();
    }
  }

  uint32_t loop_cnt = 0;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5 * 1000));

    if (!net_mgr_is_connected()) {
      continue;
    }

    // ========== 1. 心跳与状态上报 ==========
    int aroma_states[3] = {aroma_get_state(AROMA_CH_1),
                           aroma_get_state(AROMA_CH_2),
                           aroma_get_state(AROMA_CH_3)};

    // FPS 可以后续从 LVGL 查，当前给默认 30.0
    float fps = 30.0f;
    uint32_t free_mem = esp_get_free_heap_size();
    const char *current_id = "";
    if (g_photo_count > 0 && g_current_idx >= 0 &&
        g_current_idx < g_photo_count) {
      current_id = g_photos[g_current_idx].id;
    }
    photo_client_send_heartbeat(current_id, fps, free_mem, aroma_states);

    // ========== 2. 拉取遥控指令 ==========
    device_command_t cmd;
    if (photo_client_fetch_command(&cmd) == ESP_OK && cmd.has_command) {
      ESP_LOGI(TAG, "Received command: %s, target: %s, ch: %d, state: %d",
               cmd.cmd, cmd.target_id, cmd.channel, cmd.state);
      if (strcmp(cmd.cmd, "switch_photo") == 0) {
        ui_voice_next_photo();
      } else if (strcmp(cmd.cmd, "toggle_aroma") == 0) {
        aroma_set(cmd.channel, cmd.state);
      } else if (strcmp(cmd.cmd, "simulate_text") == 0) {
        char ws_json[384];
        snprintf(ws_json, sizeof(ws_json), "{\"event\":\"simulate_text\",\"text\":\"%s\"}", cmd.text);
        voice_assistant_send_text(ws_json);
      }
    }

    upload_info_t info;
    if (photo_client_check_upload(&info) == ESP_OK && info.has_upload &&
        !info.downloaded) {
      ESP_LOGI(TAG, "New upload photo detected from %s (Background)...",
               info.uploader_name);

      uint8_t *buf = NULL;
      size_t len = 0;
      // 在独立线程下载大体积图像，防止 UI 卡顿
      if (photo_client_download_upload(info.id, &buf, &len) == ESP_OK && buf) {
        ESP_LOGI(
            TAG,
            "Download upload photo success: %d bytes. Dispatching to Core 1...",
            len);

        // 采用信号量保护队列数据，避免长时间持有 lvgl_port_lock 阻塞 UI 渲染
        if (g_pending_mutex &&
            xSemaphoreTake(g_pending_mutex, portMAX_DELAY) == pdTRUE) {
          if (g_has_pending_upload && g_pending_upload.buf != NULL) {
            photo_client_free_buf(g_pending_upload.buf);
          }
          g_pending_upload.buf = buf;
          g_pending_upload.len = len;
          strncpy(g_pending_upload.message, info.message,
                  sizeof(g_pending_upload.message) - 1);
          g_pending_upload.message[sizeof(g_pending_upload.message) - 1] = '\0';
          strncpy(g_pending_upload.uploader, info.uploader_name,
                  sizeof(g_pending_upload.uploader) - 1);
          g_pending_upload.uploader[sizeof(g_pending_upload.uploader) - 1] =
              '\0';
          g_has_pending_upload = true;

          xSemaphoreGive(g_pending_mutex);
        }
      }
    }
    loop_cnt++;
  }
}

/**
 * @brief 异步前台 UI 调度器 (Core 1)
 *
 * 检查是否有挂起的上传照片需要前台渲染。
 *
 * @param t 定时器句柄
 * @return void
 */
static void upload_dispatch_timer_cb(lv_timer_t *t) {
  (void)t;
  if (g_has_pending_upload) {
    if (ui_is_upload_animating()) {
      return;
    }
    uint8_t *new_buf = NULL;
    size_t new_len = 0;
    char msg[128] = {0};
    char uploader[64] = {0};

    // 使用互斥锁保护队列的读操作
    if (g_pending_mutex &&
        xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      new_buf = g_pending_upload.buf;
      new_len = g_pending_upload.len;
      strcpy(msg, g_pending_upload.message);
      strcpy(uploader, g_pending_upload.uploader);
      g_has_pending_upload = false; // 消费掉
      xSemaphoreGive(g_pending_mutex);
    }

    if (new_buf) {
      // 防御性校验：检查图像数据包长度是否合法
      if (new_len != 768000) {
        ESP_LOGE(TAG,
                 "Asynchronous Dispatcher: Discarded invalid upload buffer %p "
                 "with len %d",
                 new_buf, new_len);
        photo_client_free_buf(new_buf); // 及时清理防止内存泄漏
        return;
      }

      ESP_LOGI(
          TAG,
          "Asynchronous UI Dispatcher: Triggering ui_show_upload on Core 1");

      // 双显存缓冲安全交接
      if (g_old_photo_buf) {
        photo_client_free_buf(g_old_photo_buf);
      }
      g_old_photo_buf = g_photo_buf; // 将当前显示的图层数据存入备份缓冲区
      g_photo_buf = new_buf;         // 主缓冲区更新为新图

      // 在 UI 线程安全触发上传通知动画
      ui_show_upload(new_buf, new_len, msg, uploader);
    }
  }
}

/**
 * @brief 自动轮换照片定时器回调
 *
 * @param t 定时器句柄
 * @return void
 */
static void photo_rotate_timer(lv_timer_t *t) {
  /* 语音助手活跃时跳过自动轮换，避免与语音切照片冲突 */
  if (va_is_active()) {
    ESP_LOGI(TAG, "Skipping rotation — voice assistant active");
    return;
  }
  if (g_photo_count == 0) {
    photo_client_fetch_today(g_photos, &g_photo_count);
  }
  if (g_photo_count > 0) {
    g_current_idx = (g_current_idx + 1) % g_photo_count;
    fetch_and_display_photo(g_current_idx);
  }
}

/**
 * @brief 传感器更新定时器回调
 *
 * @param t 定时器句柄
 * @return void
 */
static void sensor_update_timer(lv_timer_t *t) {
  float temp = 0, hum = 0;
  if (aht20_get_latest(&temp, &hum) == ESP_OK) {
    ui_update_weather(temp, hum);
  }
}

/**
 * @brief 时间更新定时器回调
 *
 * @param t 定时器句柄
 * @return void
 */
static void time_update_timer(lv_timer_t *t) { ui_update_time(); }

// --- app_ui dead thread has been removed. Initialization moved to app_ui_init. ---

/**
 * @brief 语音助手触发切换下一张照片
 *
 * 供语音任务调用，内部不锁互斥量，锁操作在 fetch_and_display_photo 内。
 * @return void
 */

void ui_voice_next_photo(void) {
  if (g_photo_count == 0) {
    photo_client_fetch_today(g_photos, &g_photo_count);
  }
  if (g_photo_count > 0) {
    g_current_idx = (g_current_idx + 1) % g_photo_count;
    fetch_and_display_photo(g_current_idx);
  } else {
    if (lvgl_port_lock(-1)) {
      ui_show_placeholder();
      lvgl_port_unlock();
    }
  }
}

void ui_voice_prev_photo(void) {
  if (g_photo_count == 0) {
    photo_client_fetch_today(g_photos, &g_photo_count);
  }
  if (g_photo_count > 0) {
    g_current_idx =
        (g_current_idx == 0) ? g_photo_count - 1 : g_current_idx - 1;
    fetch_and_display_photo(g_current_idx);
  } else {
    if (lvgl_port_lock(-1)) {
      ui_show_placeholder();
      lvgl_port_unlock();
    }
  }
}

void ui_voice_on_wake(void) {
  /* 唤醒时的 UI 反馈：短暂闪烁或显示图标 */
  if (lvgl_port_lock(100)) {
    /* 当前版本在状态栏显示唤醒指示；后续可扩展 LED 控制 */
    lvgl_port_unlock();
  }
}

void ui_voice_on_state(int state) {
  static const char *names[] = {"等待唤醒", "录音中", "LLM回复中"};
  if (state >= 0 && state < 3) {
    ESP_LOGI(TAG, "Voice state: %s", names[state]);
  }
  /* 后续可扩展：在屏幕上显示语音状态指示器 */
}

/* ── 背光亮度语音控制 ──────────────────────────────────────────── */

static uint8_t g_brightness = 100;

void ui_voice_brightness_up(void) {
  if (g_brightness < 100) {
    g_brightness = (g_brightness + 10 > 100) ? 100 : g_brightness + 10;
  }
  ESP_LOGI(TAG, "Brightness → %d%%", g_brightness);
  waveshare_rgb_lcd_bl_set_brightness(g_brightness);
}

void ui_voice_brightness_down(void) {
  if (g_brightness > 0) {
    g_brightness = (g_brightness < 10) ? 0 : g_brightness - 10;
  }
  ESP_LOGI(TAG, "Brightness → %d%%", g_brightness);
  waveshare_rgb_lcd_bl_set_brightness(g_brightness);
}

/* ── 休眠模式 ──────────────────────────────────────────────────── */

void ui_voice_sleep(void) {
  g_sleep_mode = true;
  ESP_LOGI(TAG, "Entering sleep mode: backlight off, audio stop, mist off");
  waveshare_rgb_lcd_bl_set_brightness(0);
  stream_player_stop();
  peripherals_mist_off();
  /* 暂停照片自动轮播（轮播定时器检查此标志位） */
}

void ui_voice_wake_from_sleep(void) {
  if (!g_sleep_mode) return;
  g_sleep_mode = false;
  ESP_LOGI(TAG, "Waking from sleep mode");
  waveshare_rgb_lcd_bl_set_brightness(g_brightness);
  /* 恢复自动轮播（轮播定时器重新激活） */
}

bool ui_is_sleep_mode(void) {
  return g_sleep_mode;
}

/* ── 云端 show_specific / resume_playlist ──────────────────────── */

static lv_timer_t *g_hold_timer = NULL;
static bool g_hold_until_midnight = false;

static void hold_midnight_check(lv_timer_t *t) {
  time_t now;
  time(&now);
  struct tm *lt = localtime(&now);
  if (lt->tm_hour == 0 && lt->tm_min == 0) {
    ESP_LOGI(TAG, "Midnight reached — auto resume_playlist");
    ui_resume_playlist();
  }
}

void ui_show_photo_from_url(const char *url, const char *hold_mode) {
  if (!url) return;
  ESP_LOGI(TAG, "show_specific: %s hold=%s", url, hold_mode);

  // 通过 photo_client 从 Flask 代理下载 RGB565 并显示
  // 若 URL 是 /api/photo/<id>.jpg 格式，提取 ID 并下载 RGB565
  // MVP: 尝试作为 photo ID 解析
  const char *id_start = strstr(url, "/api/photo/");
  if (id_start) {
    id_start += 11; // skip "/api/photo/"
    char photo_id[128];
    size_t len = strcspn(id_start, ".?/");
    if (len < sizeof(photo_id)) {
      memcpy(photo_id, id_start, len);
      photo_id[len] = '\0';
      ESP_LOGI(TAG, "Resolved photo ID: %s", photo_id);
      uint8_t *buf = NULL;
      size_t buf_len = 0;
      if (photo_client_download_rgb565(photo_id, &buf, &buf_len) == ESP_OK) {
        if (lvgl_port_lock(500)) {
          ui_set_photo_data(buf, buf_len, NULL, NULL, NULL);
          lvgl_port_unlock();
        }
      }
    }
  }

  // Hold mode 定时器
  if (g_hold_timer) {
    lv_timer_del(g_hold_timer);
    g_hold_timer = NULL;
  }
  g_hold_until_midnight = (hold_mode && strcmp(hold_mode, "until_midnight") == 0);
  if (g_hold_until_midnight) {
    g_hold_timer = lv_timer_create(hold_midnight_check, 60000, NULL);
    ESP_LOGI(TAG, "Hold until midnight timer started");
  }
}

void ui_resume_playlist(void) {
  ESP_LOGI(TAG, "resume_playlist");
  if (g_hold_timer) {
    lv_timer_del(g_hold_timer);
    g_hold_timer = NULL;
  }
  g_hold_until_midnight = false;
  // 停止可能正在播放的伴随音频
  stream_player_stop();
  // 恢复正常的 60 分钟自动轮播
  g_rotate_requested = true;
}

void ui_trigger_album_filter(const char *filter) {
  photo_client_set_tag(filter);
  g_photo_count = 0;
  ESP_LOGI(TAG, "UI Triggered: Refresh photos with filter: %s", filter);
  photo_client_fetch_today(g_photos, &g_photo_count);
  if (g_photo_count > 0) {
    g_current_idx = 0;
    fetch_and_display_photo(0);
  } else {
    if (lvgl_port_lock(-1)) {
      ui_show_placeholder();
      lvgl_port_unlock();
    }
  }
}

/**
 * @brief 初始化 UI 环境
 *
 * 直接同步初始化 LCD 及 LVGL，拉起相关定时器与统一网络守护进程。
 * @return void
 */
void app_ui_init(void) {
  waveshare_esp32_s3_rgb_lcd_init();
  ESP_LOGI(TAG, "LCD initialized, backlight kept ON for seamless transition from ROM white screen");
  ESP_LOGI(TAG, "Display in portrait mode (driver rotation)");

  // 2. 初始化 UI 框架（持有 LVGL 锁）
  if (lvgl_port_lock(-1)) {
    ui_main_init();
    ui_update_time(); // 初始化时先刷新一次时间
    lvgl_port_unlock();
  }

  ESP_LOGI(TAG, "First frame (White LOADING) rendered seamlessly.");

  g_pending_mutex = xSemaphoreCreateMutex();

  // 3. LVGL 定时器（lv_timer_create 必须在持锁状态下调用）
  if (lvgl_port_lock(-1)) {
    lv_timer_create(photo_rotate_timer, 60 * 60 * 1000, NULL);
    lv_timer_create(sensor_update_timer, 5000, NULL);
    lv_timer_create(time_update_timer, 1000, NULL);
    lv_timer_create(upload_dispatch_timer_cb, 100, NULL); // 100ms 巡检异步上传队列
    lvgl_port_unlock();
  }

  // 4. 启动统一的后台网络守护任务 (Network Daemon, 部署于 Core 0)
  ESP_LOGI(TAG, "Starting unified async network fetch task...");
  xTaskCreatePinnedToCore(upload_check_task, "net_daemon", 4096, NULL, 1, NULL, 0);
}
