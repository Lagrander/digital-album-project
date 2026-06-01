/*
 * voice_assistant.c — 语音助手状态机实现
 *
 * 这是整个语音助手系统的核心，实现完整的唤醒→录音→云端对话流水线。
 *
 * 状态机 (va_state_t):
 *   ST_WAITING_WAKEUP ──(WakeNet9 检测到唤醒词)──→ ST_RECORDING
 *   ST_RECORDING       ──(VAD 静音 / 缓冲区满)──→ ST_WAITING_RESP
 *   ST_WAITING_RESP    ──(LLM 音频播放完毕)──────→ ST_RECORDING (连续对话)
 *   ST_WAITING_RESP    ──(超时)─────────────────→ ST_WAITING_WAKEUP
 *
 * ESP-SR 模型:
 *   WakeNet9   — 唤醒词检测 "你好小智"（灵敏度可配 80/90/95）
 *   MultiNet7  — 中文命令词识别（下一张/上一张/打开雾化/关闭雾化/拜拜）
 *   VADNet1    — 语音活动检测（30ms 帧，600ms 静音触发录音结束）
 *   NSNet      — 噪声抑制（可选，提升嘈杂环境识别率）
 *
 * WebSocket 协议 (与 Python voice_server.py 通信):
 *   事件 →  {"event":"wake_word_detected"}    唤醒通知
 *         {"event":"recording_started"}        开始录音
 *         {"event":"recording_ended"}          录音结束 → 触发 LLM 响应
 *         {"event":"recording_cancelled"}      录音取消（太短）
 *   音频 →  二进制帧实时发送（16000Hz, 16bit, 单声道 PCM）
 *   接收 ←  二进制帧 = LLM 音频（24000Hz → 服务器重采样到 16000Hz）
 *          "ping" 文本帧 = 流结束标记
 *
 * 移植自 xiaozhi-replica main.cc（1042 行 C++ → ~470 行 C）。
 * 删减: mock_voices/ *.h（节省 ~100KB Flash）、 LED GPIO、WiFi manager。
 */

#include "voice_assistant.h"
#include "audio_buf.h"
#include "net_mgr.h"
#include "voice_io.h"
#include "ws_voice.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* ESP-SR 头文件（需要 espressif/esp-sr 组件） */
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_nsn_iface.h"
#include "esp_nsn_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_vad.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "va";

/* ── 常量 ──────────────────────────────────────────────────── */
#define VA_SAMPLE_RATE 16000       /* 统一采样率                        */
#define VA_SILENCE_FRAMES 20       /* 静音帧数阈值 ≈600ms               */
#define VA_RECORD_TIMEOUT_MS 10000 /* 连续模式无语音超时 10 秒          */
#define VA_CMD_TIMEOUT_MS 5000     /* 命令等待超时                      */

/* ── 内部状态枚举 ──────────────────────────────────────────── */
typedef enum {
  ST_WAITING_WAKEUP = 0, /* 等待唤醒词                            */
  ST_RECORDING = 1,      /* 录音中，VAD 检测 + 实时音频流发送     */
  ST_WAITING_RESP = 2,   /* 等待 LLM 音频响应播放完毕             */
} va_state_t;

/* ── 全局单例（整个系统只有一个语音助手实例）───────────────── */
static va_config_t g_cfg;   /* 配置副本                          */
static va_callbacks_t g_cb; /* 回调函数集                        */
static audio_buf_t g_ab;    /* 音频缓冲区管理器                  */
static ws_voice_t *g_ws;    /* WebSocket 客户端句柄              */
static va_state_t g_state = ST_WAITING_WAKEUP;

/* ESP-SR 句柄 */
static esp_wn_iface_t *g_wakenet;     /* WakeNet9 接口                  */
static model_iface_data_t *g_wn_data; /* WakeNet9 模型数据              */
static esp_mn_iface_t *g_multinet;    /* MultiNet7 接口                 */
static model_iface_data_t *g_mn_data; /* MultiNet7 模型数据             */
static vad_handle_t g_vad;            /* VADNet1 句柄                   */
static esp_nsn_iface_t *g_nsn;        /* NSNet 噪声抑制接口             */
static esp_nsn_data_t *g_nsn_data;    /* NSNet 模型数据                 */

/* 录音过程标志 */
static bool g_vad_speech;           /* VAD 当前帧是否检测到语音          */
static int g_vad_silence_cnt;       /* 连续静音帧计数                    */
static bool g_continuous;           /* 是否处于连续对话模式              */
static bool g_user_spoke;           /* 本次录音中用户是否说过话          */
static bool g_streaming;            /* 是否正在实时发送音频到服务器      */
static TickType_t g_rec_start_tick; /* 录音开始时刻（用于超时判断）      */

/* ── 命令词定义 ────────────────────────────────────────────── */

typedef struct {
  int id;             /* 命令 ID（VA_CMD_*）                         */
  const char *pinyin; /* 拼音字符串，用于 MultiNet7 注册              */
  const char *desc;   /* 中文描述（日志输出）                        */
} va_cmd_t;

static const va_cmd_t g_commands[] = {
    {VA_CMD_NEXT_PHOTO, "xia yi zhang", "下一张"},
    {VA_CMD_PREV_PHOTO, "shang yi zhang", "上一张"},
    {VA_CMD_MIST_ON, "da kai wu hua", "打开雾化"},
    {VA_CMD_MIST_OFF, "guan bi wu hua", "关闭雾化"},
    {VA_CMD_BYE, "bai bai", "拜拜"},
};
#define VA_CMD_COUNT (sizeof(g_commands) / sizeof(g_commands[0]))

/*
 * 根据命令 ID 查找中文描述。
 * @param id 命令 ID
 * @return 中文描述字符串，未找到返回 "未知命令"
 */
static const char *cmd_desc(int id) {
  for (int i = 0; i < (int)VA_CMD_COUNT; i++)
    if (g_commands[i].id == id)
      return g_commands[i].desc;
  return "未知命令";
}

/* ── WebSocket 事件回调 ─────────────────────────────────────── */

/*
 * WebSocket 事件处理。
 *
 * WS_VOICE_DATA_BINARY: 收到 LLM 音频数据块 → 送入流式环形缓冲区。
 *   仅在 ST_WAITING_RESP 状态时处理，自动触发 voice_io_spk_play_stream()。
 * WS_VOICE_DATA_TEXT: 检查是否为 "ping" 流结束标记 →
 *   调用 audio_buf_stream_finish() + on_response_done 回调。
 */
static void on_ws_event(const ws_voice_evt_t *evt, void *ctx) {
  (void)ctx;
  switch (evt->type) {
  case WS_VOICE_CONNECTED:
    ESP_LOGI(TAG, "WS connected");
    break;
  case WS_VOICE_DISCONNECTED:
    ESP_LOGI(TAG, "WS disconnected");
    break;
  case WS_VOICE_DATA_BINARY:
    /* 仅在等待 LLM 回复时处理二进制音频数据 */
    if (evt->data_len > 0 && g_state == ST_WAITING_RESP) {
      if (!g_ab.streaming)
        audio_buf_stream_begin(&g_ab);
      audio_buf_stream_feed(&g_ab, evt->data, evt->data_len);
    }
    break;
  case WS_VOICE_DATA_TEXT:
    if (evt->data && evt->data_len > 0) {
      /* 服务器发送 "ping" 文本帧作为音频流结束标记 */
      if (evt->data_len >= 4 && memcmp(evt->data, "ping", 4) == 0) {
        if (g_ab.streaming) {
          audio_buf_stream_finish(&g_ab);
          if (g_cb.on_response_done)
            g_cb.on_response_done(g_cb.ctx);
        }
      }
    }
    break;
  case WS_VOICE_ERROR:
    ESP_LOGW(TAG, "WS error");
    break;
  default:
    break;
  }
}

/* ── 状态机辅助函数 ────────────────────────────────────────── */

/*
 * 进入录音状态。
 *
 * 操作:
 *   1. 切换状态为 ST_RECORDING
 *   2. 启动录音缓冲区
 *   3. 重置 VAD 状态 + MultiNet 识别器
 *   4. 触发 on_state_change 回调（通知 UI）
 *
 * @param reason 触发原因描述（"wake" / "continuous" / "force"），用于日志
 */
static void enter_recording(const char *reason) {
  g_state = ST_RECORDING;
  audio_buf_record_start(&g_ab);
  g_vad_speech = false;
  g_vad_silence_cnt = 0;
  g_continuous = (reason && strstr(reason, "continuous"));
  g_user_spoke = false;
  g_streaming = false;
  g_rec_start_tick = 0;

  if (g_vad)
    vad_reset_trigger(g_vad);
  if (g_multinet && g_mn_data)
    g_multinet->clean(g_mn_data);

  if (g_cb.on_state_change)
    g_cb.on_state_change(ST_RECORDING, g_cb.ctx);

  ESP_LOGI(TAG, "Recording started (%s)", reason ? reason : "wake");
}

/*
 * 退出对话，回到等待唤醒状态。
 *
 * 操作:
 *   1. 断开 WebSocket（避免空闲连接占用资源）
 *   2. 停止录音 + 清空缓冲区
 *   3. 重置所有标志
 *   4. 触发 on_state_change 回调
 */
static void exit_dialogue(void) {
  if (g_ws)
    ws_voice_disconnect(g_ws);

  g_state = ST_WAITING_WAKEUP;
  audio_buf_record_stop(&g_ab);
  audio_buf_record_clear(&g_ab);
  g_continuous = false;
  g_user_spoke = false;
  g_rec_start_tick = 0;
  g_vad_speech = false;
  g_vad_silence_cnt = 0;

  if (g_cb.on_state_change)
    g_cb.on_state_change(ST_WAITING_WAKEUP, g_cb.ctx);
  ESP_LOGI(TAG, "Back to wake-up state");
}

/*
 * 处理本地 MultiNet7 命令。
 *
 * VA_CMD_BYE: 退出对话，回到等待唤醒状态。
 * 其他命令: 通过 on_command 回调通知上层，然后重置录音状态继续监听。
 *
 * @param cmd_id MultiNet7 识别的命令 ID
 */
static void handle_local_cmd(int cmd_id) {
  ESP_LOGI(TAG, "Local cmd: %d (%s)", cmd_id, cmd_desc(cmd_id));

  if (cmd_id == VA_CMD_BYE) {
    exit_dialogue();
    return;
  }

  /* 执行命令回调（切照片 / 雾化控制） */
  if (g_cb.on_command)
    g_cb.on_command(cmd_id, g_cb.ctx);

  /* 命令执行后继续录音（连续对话模式） */
  audio_buf_record_clear(&g_ab);
  audio_buf_record_start(&g_ab);
  g_vad_speech = false;
  g_vad_silence_cnt = 0;
  g_user_spoke = false;
  g_streaming = false;
  g_rec_start_tick = xTaskGetTickCount();
  if (g_vad)
    vad_reset_trigger(g_vad);
  if (g_multinet && g_mn_data)
    g_multinet->clean(g_mn_data);
}

/* ── ESP-SR 模型初始化 ──────────────────────────────────────── */

/*
 * 初始化所有 ESP-SR 模型。
 *
 * 加载顺序:
 *   1. 从 SPIFFS "model" 分区加载模型列表
 *   2. WakeNet9 — 唤醒词检测
 *   3. MultiNet7 CN — 中文命令词（注册自定义命令）
 *   4. VADNet1 — 语音活动检测
 *   5. NSNet — 噪声抑制（可选）
 *
 * @return ESP_OK 全部加载成功，ESP_FAIL 模型文件缺失或加载失败
 */
static esp_err_t init_sr_models(void) {
  /* 从 SPIFFS 分区加载模型列表 */
  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models) {
    ESP_LOGE(TAG,
             "Model init failed — is the 'model' SPIFFS partition flashed?");
    return ESP_FAIL;
  }

  /* ── WakeNet9 唤醒词模型 ── */
  char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
  if (!wn_name) {
    ESP_LOGE(TAG, "No WakeNet model found");
    return ESP_FAIL;
  }
  g_wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(wn_name);
  g_wn_data =
      g_wakenet->create(wn_name, g_cfg.det_mode > 0 ? g_cfg.det_mode : 90);
  ESP_LOGI(TAG, "WakeNet  : %s (mode %d)", wn_name, g_cfg.det_mode);

  /* ── MultiNet7 中文命令词模型 ── */
  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
  if (mn_name) {
    g_multinet = esp_mn_handle_from_name(mn_name);
    g_mn_data = g_multinet->create(mn_name, 6000);
    ESP_LOGI(TAG, "MultiNet : %s", mn_name);

    /* 清空默认命令词，注册自定义照片/雾化命令 */
    esp_mn_commands_clear();
    esp_mn_commands_alloc(g_multinet, g_mn_data);
    for (int i = 0; i < (int)VA_CMD_COUNT; i++)
      esp_mn_commands_add(g_commands[i].id, g_commands[i].pinyin);
    esp_mn_commands_update();
  } else {
    ESP_LOGW(TAG, "No MultiNet model — local commands disabled");
  }

  /* ── VADNet1 语音活动检测 ──
   * 参数: mode=1, 16000Hz, 30ms帧, 200ms最小语音, 1000ms最小静音 */
  g_vad = vad_create_with_param(VAD_MODE_1, VA_SAMPLE_RATE, 30, 200, 1000);
  if (!g_vad) {
    ESP_LOGW(TAG, "VAD init failed");
  }

  /* ── NSNet 噪声抑制（可选）── */
  char *nsn_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
  if (nsn_name) {
    g_nsn = (esp_nsn_iface_t *)esp_nsnet_handle_from_name(nsn_name);
    if (g_nsn)
      g_nsn_data = g_nsn->create(nsn_name);
    ESP_LOGI(TAG, "NSNet   : %s %s", nsn_name, g_nsn_data ? "on" : "off");
  }

  return ESP_OK;
}

/* ── 公开 API ───────────────────────────────────────────────── */

/*
 * 语音助手初始化。
 *
 * 执行步骤:
 *   1. 保存配置 + 回调
 *   2. 等待 Wi-Fi 连接就绪（最多 60 秒）
 *   3. 初始化 I2S 麦克风 + 扬声器
 *   4. 分配音频缓冲区（~1.4 MB）
 *   5. 创建 WebSocket 客户端并连接
 *   6. 加载 ESP-SR 模型（WakeNet9 / MultiNet7 / VADNet1 / NSNet）
 *
 * 阻塞调用，全部成功后才返回。任一环节失败返回错误码。
 */
esp_err_t va_init(const va_config_t *cfg, const va_callbacks_t *cbs) {
  if (!cfg || !cfg->ws_uri)
    return ESP_ERR_INVALID_ARG;

  g_cfg = *cfg;
  g_cb = cbs ? *cbs : (va_callbacks_t){0};
  if (g_cfg.det_mode == 0)
    g_cfg.det_mode = 90;

  /* 等待 Wi-Fi */
  ESP_LOGI(TAG, "Waiting for WiFi...");
  int retry = 0;
  while (!net_mgr_is_connected() && retry < 60) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    retry++;
  }
  if (!net_mgr_is_connected()) {
    ESP_LOGE(TAG, "WiFi not connected");
    return ESP_ERR_TIMEOUT;
  }

  /* 初始化 I2S 音频硬件 */
  ESP_ERROR_CHECK(voice_io_mic_init(VA_SAMPLE_RATE, 1, 16));
  ESP_ERROR_CHECK(voice_io_spk_init(VA_SAMPLE_RATE, 1, 16));

  /* 分配音频缓冲区 */
  audio_buf_t ab_init = {0};
  g_ab = ab_init;
  ESP_ERROR_CHECK(audio_buf_init(&g_ab));

  /* 创建 WebSocket 客户端 */
  g_ws = ws_voice_create(g_cfg.ws_uri, true, on_ws_event, NULL);
  if (!g_ws)
    return ESP_ERR_NO_MEM;
  ESP_ERROR_CHECK(ws_voice_connect(g_ws));

  /* 加载 ESP-SR 模型 */
  ESP_ERROR_CHECK(init_sr_models());

  ESP_LOGI(TAG, "Voice assistant ready. Wake word: '%s'",
           g_cfg.wake_word ? g_cfg.wake_word : "default");
  return ESP_OK;
}

/*
 * 语音助手主循环（永不返回）。
 *
 * 每帧处理流程:
 *   while(1):
 *     1. 从麦克风读取一个音频块（WakeNet chunk size）
 *     2. 可选: NSNet 噪声抑制
 *     3. switch(g_state):
 *          ST_WAITING_WAKEUP: WakeNet9 唤醒词检测 → 进入录音
 *          ST_RECORDING:      录音缓冲 + 实时发送 + VAD + MultiNet7 + 超时检查
 *          ST_WAITING_RESP:   等待流式播放完毕 → 连续对话
 *     4. 延时 1ms 让出 CPU
 */
void va_run(void) {
  /* 获取 WakeNet 每次检测所需样本数 */
  int chunksize = g_wakenet->get_samp_chunksize(g_wn_data);
  int bytes_per_chunk = chunksize * (int)sizeof(int16_t);
  int16_t *buf = malloc(bytes_per_chunk);
  int16_t *ns_out = NULL;

  if (!buf) {
    ESP_LOGE(TAG, "Fatal: audio buffer alloc");
    return;
  }

  /* 预分配 NSNet 输出缓冲区 */
  if (g_nsn && g_nsn_data) {
    int ns_chunk = g_nsn->get_samp_chunksize(g_nsn_data);
    ns_out = malloc(ns_chunk * sizeof(int16_t));
  }

  ESP_LOGI(TAG, "Main loop running, chunk=%d samples, %d bytes", chunksize,
           bytes_per_chunk);

  while (1) {
    /* ── 1. 读麦克风 ── */
    esp_err_t r = voice_io_mic_read(false, buf, bytes_per_chunk);
    if (r != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    /* ── 2. 噪声抑制（可选）── */
    int16_t *audio = buf;
    if (g_nsn && g_nsn_data && ns_out) {
      g_nsn->process(g_nsn_data, buf, ns_out);
      audio = ns_out;
    }

    /* ── 3. 状态机 ── */
    switch (g_state) {

    /* ========================================================
     * ST_WAITING_WAKEUP — 等待唤醒词
     * WakeNet9 逐帧检测 "你好小智"。
     * 检测到唤醒词: 通知服务器 + 回调 on_wake → 进入录音。
     * ======================================================== */
    case ST_WAITING_WAKEUP: {
      wakenet_state_t wn = g_wakenet->detect(g_wn_data, audio);
      if (wn == WAKENET_DETECTED) {
        ESP_LOGI(TAG, "Wake word detected!");
        if (g_cb.on_wake)
          g_cb.on_wake(g_cb.ctx);

        /* 确保 WebSocket 连接可用 */
        if (g_ws && !ws_voice_is_connected(g_ws))
          ws_voice_connect(g_ws);

        /* 通知服务器: 唤醒 + 开始录音 */
        if (g_ws && ws_voice_is_connected(g_ws)) {
          char msg[128];
          snprintf(msg, sizeof(msg),
                   "{\"event\":\"wake_word_detected\",\"ts\":%lld}",
                   (long long)(esp_timer_get_time() / 1000));
          ws_voice_send_text(g_ws, msg, 1000);
          ws_voice_send_text(g_ws, "{\"event\":\"recording_started\"}", 1000);
        }

        enter_recording("wake");
      }
      break;
    }

    /* ========================================================
     * ST_RECORDING — 录音 + 实时发送
     *
     * 并行执行以下逻辑:
     *   - 录音缓冲区追加样本
     *   - VAD 检测到语音后开始实时发送音频到服务器
     *   - 连续模式下 MultiNet7 检测本地命令词
     *   - VAD 静音达到阈值 → 停止录音 → 发送 recording_ended
     *   - 录音缓冲区满（10 秒）→ 强制停止 → 发送 recording_ended
     *   - 连续模式超时（10 秒无语音）→ 退出对话
     * ======================================================== */
    case ST_RECORDING: {
      /* 缓冲区满 → 自动结束录音 */
      if (!g_ab.recording || audio_buf_record_is_full(&g_ab)) {
        if (audio_buf_record_is_full(&g_ab)) {
          audio_buf_record_stop(&g_ab);
          if (g_ws && ws_voice_is_connected(g_ws))
            ws_voice_send_text(g_ws, "{\"event\":\"recording_ended\"}", 1000);
          g_state = ST_WAITING_RESP;
          audio_buf_resp_begin(&g_ab);
          ESP_LOGI(TAG, "Recording full, waiting for LLM response...");
        }
        break;
      }

      /* 录音缓冲区追加 */
      int samples = bytes_per_chunk / (int)sizeof(int16_t);
      audio_buf_record_feed(&g_ab, audio, samples);

      /* 实时流式发送到服务器（仅在检测到语音后开始） */
      if (g_streaming && g_ws && ws_voice_is_connected(g_ws)) {
        ws_voice_send_binary(g_ws, (const uint8_t *)audio, bytes_per_chunk,
                             500);
      }

      /* 本地命令词检测（仅连续对话模式） */
      if (g_continuous && g_multinet && g_mn_data) {
        esp_mn_state_t mn = g_multinet->detect(g_mn_data, audio);
        if (mn == ESP_MN_STATE_DETECTED) {
          esp_mn_results_t *res = g_multinet->get_results(g_mn_data);
          if (res->num > 0) {
            audio_buf_record_stop(&g_ab);
            handle_local_cmd(res->command_id[0]);
            continue;
          }
        }
      }

      /* VAD 语音活动检测 */
      if (g_vad) {
        vad_state_t vs = vad_process(g_vad, audio, VA_SAMPLE_RATE, 30);
        if (vs == VAD_SPEECH) {
          /* 检测到语音: 开始流式发送 */
          g_vad_speech = true;
          g_vad_silence_cnt = 0;
          g_user_spoke = true;
          g_rec_start_tick = 0;
          if (!g_streaming) {
            g_streaming = true;
            ESP_LOGI(TAG, "Speech detected, streaming...");
          }
        } else if (vs == VAD_SILENCE && g_vad_speech) {
          /* 语音之后的静音: 计数判断是否结束 */
          g_vad_silence_cnt++;
          if (g_vad_silence_cnt >= VA_SILENCE_FRAMES) {
            ESP_LOGI(TAG, "Silence, stopping (%.1fs)",
                     audio_buf_record_duration(&g_ab));
            audio_buf_record_stop(&g_ab);
            g_streaming = false;

            size_t rec_len = 0;
            const int16_t *rec = g_ab.rec_buf;
            rec_len = g_ab.rec_len;
            (void)rec;

            /* 录音足够长（>250ms）→ 发送到 LLM */
            if (g_user_spoke && rec_len > (size_t)(VA_SAMPLE_RATE / 4)) {
              if (g_ws && ws_voice_is_connected(g_ws))
                ws_voice_send_text(g_ws, "{\"event\":\"recording_ended\"}",
                                   1000);
              g_state = ST_WAITING_RESP;
              audio_buf_resp_begin(&g_ab);
            } else {
              /* 录音太短 → 取消，重新开始录音 */
              if (g_ws && ws_voice_is_connected(g_ws))
                ws_voice_send_text(g_ws, "{\"event\":\"recording_cancelled\"}",
                                   1000);
              audio_buf_record_clear(&g_ab);
              audio_buf_record_start(&g_ab);
              g_vad_speech = false;
              g_vad_silence_cnt = 0;
              g_user_spoke = false;
              g_rec_start_tick = g_continuous ? xTaskGetTickCount() : 0;
              if (g_vad)
                vad_reset_trigger(g_vad);
            }
          }
        }
      }

      /* 连续模式超时: 10 秒无人说话 → 退出对话 */
      if (g_continuous && g_rec_start_tick > 0 && !g_user_spoke) {
        if ((xTaskGetTickCount() - g_rec_start_tick) >
            pdMS_TO_TICKS(VA_RECORD_TIMEOUT_MS)) {
          ESP_LOGW(TAG, "No speech for %ds, exiting",
                   VA_RECORD_TIMEOUT_MS / 1000);
          audio_buf_record_stop(&g_ab);
          exit_dialogue();
        }
      }
      break;
    }

    /* ========================================================
     * ST_WAITING_RESP — 等待 LLM 响应播放完毕
     *
     * 音频数据通过 WS_VOICE_DATA_BINARY 回调异步接收，
     * 经过 audio_buf_stream_feed 边收边播。
     * 当 resp_played=true 且 streaming=false 时表示播放完毕，
     * 自动进入连续对话模式（再次录音）。
     * ======================================================== */
    case ST_WAITING_RESP: {
      if (g_ab.resp_played && !g_ab.streaming) {
        /* LLM 响应播放完成 → 通知服务器开始下一轮录音 */
        if (g_ws && ws_voice_is_connected(g_ws))
          ws_voice_send_text(g_ws, "{\"event\":\"recording_started\"}", 1000);

        if (g_cb.on_response_done)
          g_cb.on_response_done(g_cb.ctx);
        enter_recording("continuous");
      }
      break;
    }
    }

    /* 让出 CPU，避免饥饿其他任务 */
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

/*
 * 外部强制唤醒（触摸屏按钮等）。
 * 仅在等待唤醒状态时生效，等同于检测到唤醒词。
 */
void va_force_wake(void) {
  if (g_state == ST_WAITING_WAKEUP) {
    if (g_cb.on_wake)
      g_cb.on_wake(g_cb.ctx);
    enter_recording("force");
  }
}

/*
 * 查询语音助手是否忙碌。
 * UI 层用于暂停照片自动轮换。
 */
bool va_is_active(void) { return g_state != ST_WAITING_WAKEUP; }
