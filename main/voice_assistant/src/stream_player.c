#include "stream_player.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "voice_io.h"
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "stream_player";
static TaskHandle_t player_task_handle = NULL;
static bool is_playing = false;
static bool g_loop_enabled = false;
static char g_loop_url[256] = {0};
static esp_http_client_handle_t g_current_http_client = NULL;

// 每次从网络读取的块大小和缓存总大小
#define READ_BUF_SIZE 16384
static uint8_t *mp3_buf = NULL;
static int16_t *pcm_buf = NULL;
// 重采样临时缓冲区：一次性预分配，消灭热循环里的 malloc/free
static int16_t *resample_buf = NULL;

static esp_err_t _http_event_handle(esp_http_client_event_t *evt) {
  return ESP_OK;
}

static void stream_player_task(void *pvParameters) {
  char *url = (char *)pvParameters;

  // 播放新流媒体前，强制给功放进行硬件物理复位
  voice_io_spk_force_reset();

  mp3_buf =
      heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  pcm_buf = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  // 预分配重采样缓冲区（最坏情况：1152 * 2 = 2304 字节），避免热循环 malloc
  resample_buf = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!mp3_buf || !pcm_buf || !resample_buf) {
    ESP_LOGE(TAG,
             "Failed to allocate memory for mp3 decoder buffers (Internal)");
    goto cleanup;
  }

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _http_event_handle,
      .timeout_ms = 5000,
      .buffer_size = 2048,
      .buffer_size_tx = 1024,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  g_current_http_client = client;
  if (!client) {
    ESP_LOGE(TAG, "Failed to init http client");
    g_current_http_client = NULL;
    goto cleanup;
  }

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open http connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    goto cleanup;
  }

  int content_length = esp_http_client_fetch_headers(client);
  ESP_LOGI(TAG, "HTTP Stream opened, Content-Length: %d bytes", content_length);

  mp3dec_t *mp3d =
      heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!mp3d) {
    ESP_LOGE(TAG, "Failed to allocate mp3dec_t (need %u bytes in Internal RAM)",
             (unsigned)sizeof(mp3dec_t));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    goto cleanup;
  }
  mp3dec_init(mp3d);

  int bytes_in_buf = 0;
  uint32_t frame_cnt = 0;

  while (is_playing) {
    frame_cnt++;
    // 只要缓存剩余不足 8192 字节，就去网络补满到 16384 字节
    if (bytes_in_buf < 8192) {
      int to_read = READ_BUF_SIZE - bytes_in_buf;
      if (to_read > 0) {
        if (frame_cnt < 5 || frame_cnt % 50 == 0)
          ESP_LOGI(TAG, "[%lu] >> Read HTTP...", frame_cnt);
        int read_len = esp_http_client_read(
            client, (char *)mp3_buf + bytes_in_buf, to_read);
        if (frame_cnt < 5 || frame_cnt % 50 == 0)
          ESP_LOGI(TAG, "[%lu] << Read HTTP got %d", frame_cnt, read_len);
        if (read_len < 0) {
          ESP_LOGE(TAG, "HTTP Read Error");
          break;
        } else if (read_len == 0) {
          if (g_loop_enabled && is_playing) {
            ESP_LOGI(TAG, "HTTP Stream EOF — looping...");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            bytes_in_buf = 0;
            mp3dec_init(mp3d);
            // 重新建立 HTTP 连接
            client = esp_http_client_init(&config);
            g_current_http_client = client;
            if (!client || esp_http_client_open(client, 0) != ESP_OK) {
              ESP_LOGE(TAG, "Loop reconnect failed");
              break;
            }
            esp_http_client_fetch_headers(client);
            continue;
          }
          ESP_LOGI(TAG, "HTTP Stream EOF (End of file)");
          break; // 播放结束
        }
        bytes_in_buf += read_len;
      }
    }

    // 尝试解码一帧
    mp3dec_frame_info_t info;
    int samples =
        mp3dec_decode_frame(mp3d, mp3_buf, bytes_in_buf, pcm_buf, &info);

    if (samples > 0) {
      if (info.channels == 2) {
        // minimp3 返回的 samples 是【每声道的样本数】
        // 因此立体声总共有 samples * 2 个样本数据，交织存放。
        // 将它们下混音成单声道后，长度正好等于 samples！
        for (int i = 0; i < samples; i++) {
          // 将左右声道的样本相加并取平均（防止溢出），混合为单声道
          pcm_buf[i] = (int16_t)(((int32_t)pcm_buf[i * 2] + (int32_t)pcm_buf[i * 2 + 1]) / 2);
        }
      }
      if (info.hz != 16000 && info.hz > 0) {
        int out_samples = (int)((int64_t)samples * 16000 / info.hz);
        // 截断到预分配缓冲区上限，绝对防止越界
        if (out_samples > MINIMP3_MAX_SAMPLES_PER_FRAME)
          out_samples = MINIMP3_MAX_SAMPLES_PER_FRAME;
        if (out_samples > 0 && resample_buf) {
          // 使用定点数 (16.16) 代替浮点数进行极速重采样，消除 FPU 开销
          uint32_t ratio_fp = (info.hz << 16) / 16000;
          for (int i = 0; i < out_samples; i++) {
            uint32_t src_pos = i * ratio_fp;
            int idx = src_pos >> 16;
            uint32_t frac = src_pos & 0xFFFF; // 0~65535
            if (idx + 1 < samples) {
              int32_t v1 = pcm_buf[idx];
              int32_t v2 = pcm_buf[idx + 1];
              resample_buf[i] = (int16_t)(((v1 * (0x10000 - frac)) >> 16) +
                                          ((v2 * frac) >> 16));
            } else {
              resample_buf[i] = pcm_buf[idx];
            }
          }
          // 将结果拷回 pcm_buf（resample_buf 是预分配的静态缓冲，无需 free！）
          memcpy(pcm_buf, resample_buf, out_samples * sizeof(int16_t));
          samples = out_samples;
          static bool rate_logged = false;
          if (!rate_logged) {
            ESP_LOGI(TAG, "Resampling from %dHz -> 16000Hz (ratio %.2f)",
                     info.hz, (float)info.hz / 16000.0f);
            rate_logged = true;
          }
        }
      }

      // 将 16kHz 单声道 PCM 推送到底层全双工缓冲池
      int max_amp = 0;
      for (int i = 0; i < samples; i++) {
        int abs_val = pcm_buf[i] < 0 ? -pcm_buf[i] : pcm_buf[i];
        if (abs_val > max_amp)
          max_amp = abs_val;
      }
      if (frame_cnt < 5 || frame_cnt % 50 == 0)
        ESP_LOGI(TAG, "[%lu] >> Push I2S %d bytes, Max Amplitude: %d",
                 frame_cnt, samples * sizeof(int16_t), max_amp);
      esp_err_t push_ret = voice_io_spk_play_stream((uint8_t *)pcm_buf,
                                                    samples * sizeof(int16_t));
      if (push_ret != ESP_OK) {
        ESP_LOGW(TAG, "[%lu] Push I2S failed: %s (amp or ringbuf issue)",
                 frame_cnt, esp_err_to_name(push_ret));
      }
      if (frame_cnt < 5 || frame_cnt % 50 == 0)
        ESP_LOGI(TAG, "[%lu] << Push I2S done", frame_cnt);

      // 将已经被解码过的数据丢弃，把剩下的数据往前挪
      bytes_in_buf -= info.frame_bytes;
      memmove(mp3_buf, mp3_buf + info.frame_bytes, bytes_in_buf);

    } else if (info.frame_bytes == 0) {
      // 由于保证了传给解码器的数据至少有 2048 字节，
      // 如果它还解不出，说明这绝对是个伪帧头或脏数据！
      // 此时直接在内存中丢弃 1 字节快速滑窗。因为数量充足，不会频繁触发读网络！
      if (bytes_in_buf > 0) {
        bytes_in_buf--;
        memmove(mp3_buf, mp3_buf + 1, bytes_in_buf);
      }
      vTaskDelay(1);
    } else {
      // 跳过脏数据帧或极长的 MP3 封面信息 (防止 bytes_in_buf 负数越界崩溃)
      int skip_bytes = info.frame_bytes;
      if (skip_bytes > bytes_in_buf) {
        int remaining_to_skip = skip_bytes - bytes_in_buf;
        bytes_in_buf = 0;
        char dump_buf[512];
        while (remaining_to_skip > 0 && is_playing) {
          int to_read = (remaining_to_skip > sizeof(dump_buf))
                            ? sizeof(dump_buf)
                            : remaining_to_skip;
          int r = esp_http_client_read(client, dump_buf, to_read);
          if (r <= 0)
            break;
          remaining_to_skip -= r;
        }
      } else {
        bytes_in_buf -= skip_bytes;
        memmove(mp3_buf, mp3_buf + skip_bytes, bytes_in_buf);
      }
      vTaskDelay(1);
    }
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  voice_io_spk_stop(); // 通知底层功放进入深度休眠
  if (mp3d)
    free(mp3d);

cleanup:
  g_current_http_client = NULL;
  if (mp3_buf) {
    free(mp3_buf);
    mp3_buf = NULL;
  }
  if (pcm_buf) {
    free(pcm_buf);
    pcm_buf = NULL;
  }
  if (resample_buf) {
    free(resample_buf);
    resample_buf = NULL;
  }
  free(url);
  is_playing = false;
  player_task_handle = NULL;
  ESP_LOGI(TAG, "Stream player task finished & memory freed.");
  vTaskDelete(NULL);
}

void stream_player_play_url(const char *url) {
  g_loop_enabled = false;
  g_loop_url[0] = '\0';

  if (is_playing) {
    ESP_LOGW(TAG, "Already playing! Please stop first.");
    return;
  }

  char *url_copy = strdup(url);
  if (!url_copy) {
    ESP_LOGE(TAG, "No mem for URL copy");
    return;
  }

  is_playing = true;
  BaseType_t ret = xTaskCreate(stream_player_task, "stream_player", 8192, url_copy,
                               5, &player_task_handle);
  if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create stream_player_task (Internal RAM full?)");
      player_task_handle = NULL;
      is_playing = false;
      free(url_copy);
  }
}

void stream_player_play_url_with_loop(const char *url, bool loop) {
  if (is_playing) {
    ESP_LOGW(TAG, "Already playing! Please stop first.");
    return;
  }

  g_loop_enabled = loop;
  if (url && strlen(url) < sizeof(g_loop_url)) {
    strcpy(g_loop_url, url);
  }

  char *url_copy = strdup(url);
  if (!url_copy) {
    ESP_LOGE(TAG, "No mem for URL copy");
    return;
  }

  is_playing = true;
  // minimp3 的 mp3d_DCT_II / mp3d_synth_granule 在栈上分配大量 float 局部数组
  // 配合 Xtensa SIMD 浮点运算，实测需要 >20KB 栈！16KB 必然溢出导致 DoubleException。
  // 同时改到 Core 1，避免与 Core 0 上的 LVGL 渲染任务争抢 CPU。
  BaseType_t ret = xTaskCreatePinnedToCore(
      stream_player_task, "stream_player", 24576, url_copy, 5, &player_task_handle, 1);

  if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create stream_player task (Internal RAM exhausted)");
      is_playing = false;
      free(url_copy);
      return;
  }
}

void stream_player_stop(void) {
  if (is_playing) {
    is_playing =
        false; // 任务内部的 while 循环检测到 false 会自动退出并清理内存
    if (g_current_http_client) {
      esp_http_client_close(g_current_http_client);
    }
  }
}

bool stream_player_is_playing(void) {
  return is_playing;
}