/*
 * voice_assistant.c — 语音助手状态机实现
 *
 * 这是整个语音助手系统的核心，实现完整的唤醒→录音→云端对话流水线。
 *
 * 状态机 (va_state_t):
 *   ST_WAITING_WAKEUP ──(WakeNet9 检测到唤醒词)──→ ST_RECORDING
 *   ST_RECORDING       ──(VAD 静音 / 缓冲区满)──→ ST_WAITING_RESP
 *   ST_WAITING_RESP    ──(TTS 播放中用户说话)────→ ST_RECORDING (打断)
 *   ST_WAITING_RESP    ──(TTS 播放完毕)──────────→ ST_WAITING_TURN (倾听)
 *   ST_WAITING_RESP    ──(超时)─────────────────→ ST_WAITING_WAKEUP
 *   ST_WAITING_TURN    ──(VAD 检测到语音)────────→ ST_RECORDING (无需唤醒词)
 *   ST_WAITING_TURN    ──(超时 8s)──────────────→ ST_WAITING_WAKEUP
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
#include "../../lv_ui/src/lv_voice_assistant.h"
#include "audio_buf.h"
#include "cJSON.h"
#include "lvgl_ui_task.h"
#include "net_mgr.h"
#include "peripherals_task.h"
#include "stream_player.h"
#include "voice_io.h"
#include "waveshare_rgb_lcd_port.h"
#include "ws_voice.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "stream_player.h"

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
#include <math.h> /* sqrt 浮点开方 */

static const char *TAG = "va";

extern void ui_trigger_album_filter(const char *filter);

/* ── 常量 ──────────────────────────────────────────────────── */
#define VA_SAMPLE_RATE 16000 /* 统一采样率                        */
#define VA_SILENCE_FRAMES 20 /* 静音帧数阈值 ≈600ms，快速响应               */
#define VA_SILENCE_FRAMES_EARLY                                                \
  28 /* 录音<2s 时更宽容 ≈840ms，避免长句子被截断 */
#define VA_RECORD_TIMEOUT_MS 10000 /* 连续模式无语音超时 10 秒          */
#define VA_RESP_TIMEOUT_MS 60000   /* 等待 LLM 响应超时 60 秒           */
#define VA_CMD_TIMEOUT_MS 5000     /* 命令等待超时                      */
#define VA_TURN_TIMEOUT_MS 30000   /* 多轮对话等待下一句超时 30 秒       */
#define VA_INTERRUPT_MIN_PLAY_MS                                               \
  800                             /* VAD 打断最小播放时间，防止喇叭回声误触发 */
#define VA_TURN_RMS_THRESHOLD 100 /* 倾听模式触发录音的最小 RMS 能量     */
#define VA_WAKE_COOLDOWN_MS 2000  /* 唤醒词检测冷却时间，避免重复触发 */

/* ── 内部状态枚举 ──────────────────────────────────────────── */
typedef enum {
  ST_WAITING_WAKEUP = 0, /* 等待唤醒词                            */
  ST_RECORDING = 1,      /* 录音中，VAD 检测 + 实时音频流发送     */
  ST_WAITING_RESP = 2,   /* 等待 LLM 音频响应播放完毕             */
  ST_WAITING_TURN = 3,   /* 多轮对话：等待用户说下一句 (VAD监听)  */
} va_state_t;

/* cJSON PSRAM 分配器 wrapper：cJSON malloc_fn 签名只需 size_t */
static void *cjson_psram_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ── 全局单例（整个系统只有一个语音助手实例）───────────────── */
static va_config_t g_cfg;   /* 配置副本                          */
static va_callbacks_t g_cb; /* 回调函数集                        */
static audio_buf_t g_ab;    /* 音频缓冲区管理器                  */
static ws_voice_t *g_ws;    /* WebSocket 客户端句柄              */
static va_state_t g_state = ST_WAITING_WAKEUP;
static SemaphoreHandle_t s_ws_mutex = NULL;
static TickType_t g_wake_cooldown_end;
static TickType_t g_turn_start_tick; /* 多轮对话等待计时                */
static volatile bool g_ws_should_disconnect = false;

/* 前向声明 */
static void exit_dialogue(void);
static void exit_dialogue_ex(bool disconnect_ws);

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
static int g_vad_silence_cnt = 0;        /* 连续静音帧计数                    */
static bool g_is_web_simulated = false;  /* 是否为 Web UI 模拟输入的文本      */
static bool g_continuous;           /* 是否处于连续对话模式              */
static bool g_user_spoke;           /* 本次录音中用户是否说过话          */
static bool g_streaming;            /* 是否正在实时发送音频到服务器      */
static TickType_t g_rec_start_tick; /* 录音开始时刻（用于超时判断）      */
static TickType_t g_last_ws_keepalive_tick; /* 上次 WS 保活心跳时刻 */
static TickType_t g_resp_start_tick;     /* 等待 LLM 响应开始时刻             */
static volatile bool g_ws_error_pending; /* WS 错误待处理，由主循环安全执行 */

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

    {VA_CMD_VOL_UP, "da sheng yi dian", "大声一点"},
    {VA_CMD_VOL_DOWN, "xiao sheng yi dian", "小声一点"},
    {VA_CMD_BRIGHT_UP, "tiao liang yi dian", "调亮一点"},
    {VA_CMD_BRIGHT_DOWN, "tiao an yi dian", "调暗一点"},
    {VA_CMD_MIST_LEVEL_UP, "jia da pen wu", "加大喷雾"},
    {VA_CMD_MIST_LEVEL_DOWN, "jian xiao pen wu", "减小喷雾"},
    {VA_CMD_AUDIO_PLAY, "bo fang yin yue", "播放音乐"},
    {VA_CMD_AUDIO_STOP, "ting zhi yin yue", "停止播放"},
    {VA_CMD_MODE_SLEEP, "shui jue mo shi", "睡觉模式"},
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

static int safe_send_text(const char *text, int timeout_ms) {
  if (!g_ws || !ws_voice_is_connected(g_ws)) {
    return -1;
  }
  int ret = -1;
  if (s_ws_mutex &&
      xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    ret = ws_voice_send_text(g_ws, text, timeout_ms);
    xSemaphoreGive(s_ws_mutex);
  }
  return ret;
}

static int safe_send_binary(const uint8_t *data, size_t len, int timeout_ms) {
  if (!g_ws || !ws_voice_is_connected(g_ws)) {
    return -1;
  }
  int ret = -1;
  if (s_ws_mutex &&
      xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    ret = ws_voice_send_binary(g_ws, data, len, timeout_ms);
    xSemaphoreGive(s_ws_mutex);
  }
  return ret;
}

/*
 * ── action_dispatch: 云端 CJSON 协议解析与执行 ──────────────────
 *
 * 支持两种协议格式:
 *   [新] 功能.md 4.3 完整协议: action.mist.command, action.audio.command,
 * action.screen.command [旧] 遗留格式: action.mist(bool),
 * action.brightness(int), action.album_filter(str), media.url
 *
 * 新协议字段:
 *   dialogue.tts_text  → TTS 语音文本（TBD: 通过 WebSocket 请求 Flask 生成 TTS
 * 音频） mist.command   "on"|"off"|"keep" mist.channel
 * "mint"|"jasmine"|"rose"|"none" mist.level     1-3 audio.command
 * "play"|"stop"|"keep" audio.url      直链或 <search:> 宏（由 Flask
 * 解析后下发） audio.loop     true|false audio.volume   0-100 screen.command
 * "show_specific"|"resume_playlist"|"keep" screen.url     直链 URL
 *   screen.hold_mode  "until_midnight"|"none"
 *   screen.brightness 0-100
 */
static void action_dispatch(cJSON *root) {
  if (!root)
    return;

  /* ── 0. dialogue TTS 文本 ── */
  cJSON *dialogue = cJSON_GetObjectItem(root, "dialogue");
  if (dialogue && cJSON_IsObject(dialogue)) {
    cJSON *tts = cJSON_GetObjectItem(dialogue, "tts_text");
    cJSON *emotion = cJSON_GetObjectItem(dialogue, "emotion");
    const char *tts_str = (tts && tts->valuestring) ? tts->valuestring : "";
    const char *emo_str =
        (emotion && emotion->valuestring) ? emotion->valuestring : "neutral";
    if (tts_str[0]) {
      ESP_LOGI(TAG, "TTS text: %s", tts_str);
      lv_va_show_text(NULL, tts_str, emo_str);
    }
  }

  cJSON *action = cJSON_GetObjectItem(root, "action");
  if (!action || !cJSON_IsObject(action)) {
    // 遗留格式: media.url 顶层直接下发
    cJSON *media = cJSON_GetObjectItem(root, "media");
    if (media && cJSON_IsObject(media)) {
      cJSON *url = cJSON_GetObjectItem(media, "url");
      if (url && url->valuestring && strlen(url->valuestring) > 0) {
        ESP_LOGI(TAG, "Playing media URL: %s", url->valuestring);
        stream_player_play_url(url->valuestring);
      }
    }
    return;
  }

  /* ════════════════════════════════════════════════════════════════
   * ── 1. mist 香薰控制 ──────────────────────────────────────────
   * ════════════════════════════════════════════════════════════════ */
  cJSON *mist = cJSON_GetObjectItem(action, "mist");
  if (mist && cJSON_IsObject(mist)) {
    /* [新协议] mist.command = "on"|"off"|"keep" */
    cJSON *cmd = cJSON_GetObjectItem(mist, "command");
    if (cmd && cmd->valuestring) {
      if (strcmp(cmd->valuestring, "on") == 0) {
        cJSON *ch = cJSON_GetObjectItem(mist, "channel");
        cJSON *lv = cJSON_GetObjectItem(mist, "level");
        int channel = peripherals_mist_channel_from_string(
            ch && ch->valuestring ? ch->valuestring : "none");
        int level = (lv && cJSON_IsNumber(lv)) ? lv->valueint : 2;
        if (level < 1)
          level = 1;
        if (level > 3)
          level = 3;
        ESP_LOGI(TAG, "Mist ON  ch=%d lv=%d", channel, level);
        peripherals_set_aroma(channel, true, level);
        for (int i = 0; i < 3; i++) {
          if (i != channel)
            peripherals_set_aroma(i, false, 3);
        }
        // 如果湿度过高导致被禁，通过遥测告知服务器
        if (peripherals_is_humidity_blocked()) {
          char msg[96];
          snprintf(msg, sizeof(msg), "{\"event\":\"humidity_blocked\"}");
          voice_assistant_send_text(msg);
        }
      } else if (strcmp(cmd->valuestring, "off") == 0) {
        ESP_LOGI(TAG, "Mist OFF");
        peripherals_mist_off();
      }
      /* "keep" → do nothing */
    }
  } else if (mist && cJSON_IsBool(mist)) {
    /* [旧协议] action.mist = true/false */
    cJSON *mist_ch = cJSON_GetObjectItem(action, "mist_channel");
    cJSON *mist_lv = cJSON_GetObjectItem(action, "mist_level");
    if (cJSON_IsTrue(mist)) {
      int ch = (mist_ch && cJSON_IsNumber(mist_ch)) ? mist_ch->valueint : 0;
      int lv = (mist_lv && cJSON_IsNumber(mist_lv)) ? mist_lv->valueint : 3;
      peripherals_set_aroma(ch, true, lv);
      for (int i = 0; i < 3; i++)
        if (i != ch)
          peripherals_set_aroma(i, false, 3);
    } else {
      peripherals_mist_off();
    }
  }

  /* ════════════════════════════════════════════════════════════════
   * ── 2. audio 音频控制 ──────────────────────────────────────────
   * ════════════════════════════════════════════════════════════════ */
  cJSON *audio = cJSON_GetObjectItem(action, "audio");
  if (audio && cJSON_IsObject(audio)) {
    cJSON *cmd = cJSON_GetObjectItem(audio, "command");
    if (cmd && cmd->valuestring) {
      if (strcmp(cmd->valuestring, "play") == 0) {
        cJSON *url = cJSON_GetObjectItem(audio, "url");
        cJSON *loop = cJSON_GetObjectItem(audio, "loop");
        cJSON *vol = cJSON_GetObjectItem(audio, "volume");
        if (vol && cJSON_IsNumber(vol))
          voice_io_set_spk_volume((uint8_t)vol->valueint);
        if (url && url->valuestring && strlen(url->valuestring) > 0) {
          bool do_loop = (loop && cJSON_IsTrue(loop));
          ESP_LOGI(TAG, "Audio PLAY url=%s loop=%d", url->valuestring, do_loop);
          stream_player_play_url_with_loop(url->valuestring, do_loop);
        }
      } else if (strcmp(cmd->valuestring, "stop") == 0) {
        ESP_LOGI(TAG, "Audio STOP");
        stream_player_stop();
      }
      /* "keep" → do nothing */
    }
  }

  /* ════════════════════════════════════════════════════════════════
   * ── 3. screen 屏幕控制 ────────────────────────────────────────
   * ════════════════════════════════════════════════════════════════ */
  cJSON *screen = cJSON_GetObjectItem(action, "screen");
  if (screen && cJSON_IsObject(screen)) {
    cJSON *cmd = cJSON_GetObjectItem(screen, "command");
    if (cmd && cmd->valuestring) {
      if (strcmp(cmd->valuestring, "show_specific") == 0) {
        cJSON *url = cJSON_GetObjectItem(screen, "url");
        cJSON *hold = cJSON_GetObjectItem(screen, "hold_mode");
        const char *hold_mode =
            (hold && hold->valuestring) ? hold->valuestring : "none";
        if (url && url->valuestring && strlen(url->valuestring) > 0) {
          ESP_LOGI(TAG, "Screen SHOW_SPECIFIC url=%s hold=%s", url->valuestring,
                   hold_mode);
          ui_show_photo_from_url(url->valuestring, hold_mode);
        }
      } else if (strcmp(cmd->valuestring, "resume_playlist") == 0) {
        ESP_LOGI(TAG, "Screen RESUME_PLAYLIST");
        ui_resume_playlist();
      }
      /* "keep" → do nothing */
    }

    /* brightness 可在 screen 对象内部，也可在 action 顶层 */
    cJSON *bri = cJSON_GetObjectItem(screen, "brightness");
    if (bri && cJSON_IsNumber(bri)) {
      waveshare_rgb_lcd_bl_set_brightness((uint8_t)bri->valueint);
    }
  }

  /* ── 遗留: action.brightness (顶层) ── */
  cJSON *brightness = cJSON_GetObjectItem(action, "brightness");
  if (brightness && cJSON_IsNumber(brightness)) {
    waveshare_rgb_lcd_bl_set_brightness((uint8_t)brightness->valueint);
  }

  /* ── 遗留: action.album_filter ── */
  cJSON *album_filter = cJSON_GetObjectItem(action, "album_filter");
  if (album_filter && album_filter->valuestring &&
      strlen(album_filter->valuestring) > 0) {
    ui_trigger_album_filter(album_filter->valuestring);
  }
}

/* ── WebSocket 事件回调 ─────────────────────────────────────── */

/*
 * WebSocket 事件处理。
 *
 * 流式协议:
 *   ← binary = TTS PCM 音频块（piper-tts --output-raw, 22050Hz 16bit mono）
 *   ← {"event":"asr_final","text":"..."} = 最终 ASR 结果
 *   ← {"event":"llm_delta","text":"..."} = LLM 流式 token
 *   ← {"type":"tts_start","format":"pcm","rate":22050} = TTS 格式头
 *   ← "ping" = TTS 音频流结束标记
 *   ← CJSON (dialogue + action) = 硬件控制指令
 */
static void on_ws_event(const ws_voice_evt_t *evt, void *ctx) {
  (void)ctx;
  switch (evt->type) {
  case WS_VOICE_CONNECTED:
    ESP_LOGI(TAG, "WS connected");
    break;
  case WS_VOICE_DISCONNECTED:
    ESP_LOGI(TAG, "WS disconnected");
    if (g_state != ST_WAITING_WAKEUP) {
      ESP_LOGW(TAG,
               "WS disconnected during active dialogue, deferring exit...");
      g_ws_error_pending = true;
    }
    break;
  case WS_VOICE_DATA_BINARY:
    /* 流式 TTS 音频：RECORDING 和 WAITING_RESP 状态都接受 */
    if (evt->data_len > 0 &&
        (g_state == ST_WAITING_RESP || g_state == ST_RECORDING)) {
      if (!g_ab.streaming)
        audio_buf_stream_begin(&g_ab);
      audio_buf_stream_feed(&g_ab, evt->data, evt->data_len);
    }
    break;
  case WS_VOICE_DATA_TEXT:
    if (evt->data && evt->data_len > 0) {
      static char *s_text_buf = NULL;
      static size_t s_text_buf_len = 0;
      static char s_llm_text[256] = {0}; /* LLM 流式 token 累积缓冲区 */

      // 1.
      // 如果是新的一帧，且先前缓冲区没释放（比如发生丢包或状态异常），进行安全清理
      if (evt->payload_offset == 0 && s_text_buf != NULL) {
        free(s_text_buf);
        s_text_buf = NULL;
        s_text_buf_len = 0;
      }

      // 2. 为当前大文本帧分配缓冲区（只需在首包分配）
      // 大 JSON 强制分配到 PSRAM，避免内部 SRAM 耗尽导致分配失败 → JSON parse
      // fail pos -1
      if (s_text_buf == NULL) {
        s_text_buf = heap_caps_malloc(evt->payload_len + 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_text_buf == NULL) {
          ESP_LOGE(TAG, "Failed to allocate text assembly buffer (PSRAM)");
          break;
        }
      }

      // 3. 将当前分片数据复制到缓冲区的对应偏移位置
      if (evt->payload_offset + evt->data_len <= evt->payload_len) {
        memcpy(s_text_buf + evt->payload_offset, evt->data, evt->data_len);
        s_text_buf_len = evt->payload_offset + evt->data_len;
      } else {
        ESP_LOGE(TAG, "Payload offset out of bounds: %u + %u > %u",
                 (unsigned)evt->payload_offset, (unsigned)evt->data_len,
                 (unsigned)evt->payload_len);
        free(s_text_buf);
        s_text_buf = NULL;
        s_text_buf_len = 0;
        break;
      }

      // 4. 当数据块全部接收完毕后进行处理
      if (s_text_buf_len == evt->payload_len) {
        s_text_buf[evt->payload_len] = '\0';

        if (evt->payload_len >= 4 && memcmp(s_text_buf, "ping", 4) == 0) {
          /* TTS 音频流结束 */
          if (g_ab.streaming) {
            audio_buf_stream_finish(&g_ab);
            g_ab.resp_played = true; // 唤醒状态机，允许切回录音状态
            if (g_cb.on_response_done)
              g_cb.on_response_done(g_cb.ctx);
          }
          printf("\n\n");
          fflush(stdout);
        } else {
          cJSON *root = cJSON_Parse(s_text_buf);
          if (root) {
            cJSON *event = cJSON_GetObjectItem(root, "event");
            cJSON *text = cJSON_GetObjectItem(root, "text");
            cJSON *type = cJSON_GetObjectItem(root, "type");

            if (type && type->valuestring &&
                strcmp(type->valuestring, "tts_start") == 0) {
              ESP_LOGI(TAG, "TTS stream starting (piper-tts)");
              cJSON *sim = cJSON_GetObjectItem(root, "simulated");
              g_is_web_simulated = (sim && cJSON_IsTrue(sim));
              
              if (g_state == ST_WAITING_WAKEUP || g_state == ST_RECORDING || g_state == ST_WAITING_TURN) {
                  if (g_state == ST_RECORDING || g_state == ST_WAITING_TURN) {
                      ESP_LOGI(TAG, "Preemptively stopping recording because TTS started");
                      audio_buf_record_stop(&g_ab);
                  }
                  g_state = ST_WAITING_RESP;
                  g_resp_start_tick = xTaskGetTickCount();
                  audio_buf_resp_begin(&g_ab);
              }
              cJSON_Delete(root);
            } else if (event && event->valuestring) {
              /* ASR 最终结果: {"event":"asr_final","text":"..."} */
              if (strcmp(event->valuestring, "asr_failed") == 0) {
                ESP_LOGW(TAG,
                         "ASR failed on server, exiting dialogue (keep WS)");
                cJSON_Delete(root);
                exit_dialogue_ex(false);
              } else {
                if (text && text->valuestring) {
                  if (strcmp(event->valuestring, "asr_completed") == 0 ||
                      strcmp(event->valuestring, "asr_final") == 0) {
                    printf("\n🗣️  [ASR] %s\n", text->valuestring);
                    fflush(stdout);
                    lv_va_show_text(text->valuestring, NULL, NULL);
                    s_llm_text[0] = '\0'; /* 清空 LLM 累积，准备新一轮回复 */
                  }
                  /* LLM 流式 token: {"event":"llm_delta","text":"..."} */
                  else if (strcmp(event->valuestring, "llm_delta") == 0) {
                    printf("%s", text->valuestring);
                    fflush(stdout);
                    /* 累积 token 并更新 LVGL 显示 */
                    if (text && text->valuestring &&
                        strlen(s_llm_text) < sizeof(s_llm_text) - 1) {
                      strncat(s_llm_text, text->valuestring,
                              sizeof(s_llm_text) - strlen(s_llm_text) - 1);
                      lv_va_show_text(NULL, s_llm_text, NULL);
                    }
                  }
                }
                action_dispatch(root);
                cJSON_Delete(root);
              }
            } else {
              /* 没有 event/type 字段 → 可能是 CJSON 控制帧或 action 帧 */
              action_dispatch(root);
              cJSON_Delete(root);
            }
          } else {
            const char *err = cJSON_GetErrorPtr();
            int pos = err ? (int)(err - s_text_buf) : -1;
            char hex_pre[25] = {0};
            for (int i = 0; i < 8 && i < (int)evt->payload_len; i++)
              snprintf(hex_pre + i * 3, 4, "%02X ", ((uint8_t *)s_text_buf)[i]);
            ESP_LOGE(TAG, "JSON parse fail at pos %d/%.*s hex:%s", pos,
                     (int)evt->payload_len, s_text_buf, hex_pre);
          }
        }

        // 释放临时大缓存
        free(s_text_buf);
        s_text_buf = NULL;
        s_text_buf_len = 0;
      }
    }
    break;
  case WS_VOICE_ERROR:
    ESP_LOGW(TAG, "WS error");
    if (g_state != ST_WAITING_WAKEUP) {
      ESP_LOGW(TAG, "WS error during active dialogue, deferring exit...");
      g_ws_error_pending = true;
    }
    break;
  default:
    break;
  }
}
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
  voice_io_mic_clear();
  audio_buf_record_start(&g_ab);
  g_vad_speech = false;
  g_vad_silence_cnt = 0;
  g_continuous =
      (reason && (strstr(reason, "continuous") || strstr(reason, "turn") ||
                  strstr(reason, "interrupt")));
  g_user_spoke = false;
  g_streaming = false;
  g_rec_start_tick = 0;
  g_last_ws_keepalive_tick = xTaskGetTickCount();

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
static void exit_dialogue_ex(bool disconnect_ws) {
  static bool exiting = false;
  if (exiting)
    return;
  exiting = true;
  if (disconnect_ws && g_ws)
    g_ws_should_disconnect = true;

  /* 不再调用 g_wakenet->clean()，改为冷却期 + 唤醒后 clean 方案 */

  g_state = ST_WAITING_WAKEUP;
  g_wake_cooldown_end =
      xTaskGetTickCount() + pdMS_TO_TICKS(VA_WAKE_COOLDOWN_MS);

  audio_buf_record_stop(&g_ab);
  audio_buf_record_clear(&g_ab);
  g_continuous = false;
  g_user_spoke = false;
  g_rec_start_tick = 0;
  g_vad_speech = false;
  g_vad_silence_cnt = 0;

  if (g_cb.on_state_change)
    g_cb.on_state_change(ST_WAITING_WAKEUP, g_cb.ctx);
  ESP_LOGI(TAG, "Back to wake-up state (disconnect_ws=%d)", disconnect_ws);

  exiting = false;
}

static void exit_dialogue(void) { exit_dialogue_ex(true); }

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
esp_err_t va_preload_models(int det_mode) {
  /* ── 前置校验：检查 model 分区数据是否合法，避免 mmap 损坏数据触发 MMU Cache
   * error ── */
  const esp_partition_t *model_part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "model");
  if (!model_part) {
    ESP_LOGW(TAG, "No 'model' partition in partition table");
    return ESP_FAIL;
  }
  uint8_t hdr[16];
  if (esp_partition_read(model_part, 0, hdr, sizeof(hdr)) != ESP_OK) {
    ESP_LOGW(TAG, "Cannot read model partition header");
    return ESP_FAIL;
  }
  bool all_ff = true, all_00 = true;
  for (int i = 0; i < (int)sizeof(hdr); i++) {
    if (hdr[i] != 0xFF)
      all_ff = false;
    if (hdr[i] != 0x00)
      all_00 = false;
  }
  if (all_ff || all_00) {
    ESP_LOGW(TAG,
             "model partition appears empty/unflashed (hdr[0]=0x%02x)"
             " — please flash srmodels.bin to 0x%lx",
             hdr[0], (unsigned long)model_part->address);
    return ESP_FAIL;
  }

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
      g_wakenet->create(wn_name, det_mode); // 传入 0 即 DET_MODE_90 最高灵敏度
  ESP_LOGI(TAG, "WakeNet  : %s (mode %d)", wn_name, det_mode);

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
    /* 同义拼音：同一命令 ID 支持多种说法 */
    esp_mn_commands_add(VA_CMD_MIST_OFF, "guan bi xiang xun");
    esp_mn_commands_add(VA_CMD_MIST_OFF, "guan bi pen wu");
    esp_mn_commands_add(VA_CMD_AUDIO_STOP, "guan diao sheng yin");
    esp_mn_commands_update();
  } else {
    ESP_LOGW(TAG, "No MultiNet model — local commands disabled");
  }

  /* ── VADNet1 语音活动检测 ──
   * 参数: mode=1, 16000Hz, 30ms帧, 200ms最小语音, 1000ms最小静音 */
  g_vad = vad_create_with_param(VAD_MODE_4, VA_SAMPLE_RATE, 30, 200, 1000);
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

  if (s_ws_mutex == NULL) {
    s_ws_mutex = xSemaphoreCreateMutex();
  }

  cJSON_Hooks cjson_hooks = {
      .malloc_fn = cjson_psram_malloc,
      .free_fn = free,
  };
  cJSON_InitHooks(&cjson_hooks);

  /* 1. 等待 Wi-Fi */
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
  vTaskDelay(pdMS_TO_TICKS(1500));

  /* 2. 创建 WebSocket 客户端 */
  g_ws = ws_voice_create(g_cfg.ws_uri, true, on_ws_event, NULL);
  if (!g_ws)
    return ESP_ERR_NO_MEM;
  esp_err_t ws_ret = ws_voice_connect(g_ws);
  if (ws_ret != ESP_OK) {
    ESP_LOGW(TAG, "WebSocket connect failed (ret=%d), will retry in background",
             ws_ret);
  }

  /* 3. 初始化 I2S 音频硬件 */
  ESP_ERROR_CHECK(voice_io_mic_init(VA_SAMPLE_RATE, 1, 16));
  ESP_ERROR_CHECK(voice_io_spk_init(VA_SAMPLE_RATE, 1, 16));

  /* 4. 分配音频缓冲区 */
  audio_buf_t ab_init = {0};
  g_ab = ab_init;
  ESP_ERROR_CHECK(audio_buf_init(&g_ab));

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

  int16_t *buf = heap_caps_aligned_alloc(16, bytes_per_chunk,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *ns_out = NULL;

  if (!buf) {
    ESP_LOGE(TAG, "Fatal: audio buffer alloc");
    return;
  }

  /* 预分配 NSNet 输出缓冲区 */
  if (g_nsn && g_nsn_data) {
    int ns_chunk = g_nsn->get_samp_chunksize(g_nsn_data);
    ns_out = heap_caps_aligned_alloc(16, ns_chunk * sizeof(int16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  ESP_LOGI(TAG, "Main loop running, chunk=%d samples, %d bytes", chunksize,
           bytes_per_chunk);

  while (1) {
    /* ── 0. WS 错误待处理（从回调外安全执行，避免重入崩溃）── */
    if (g_ws_error_pending) {
      g_ws_error_pending = false;
      if (g_state != ST_WAITING_WAKEUP) {
        ESP_LOGW(TAG, "Handling deferred WS error exit...");
        exit_dialogue_ex(false);
      }
      continue;
    }
    if (g_ws_should_disconnect) {
      g_ws_should_disconnect = false;
      if (g_ws) {
        ESP_LOGI(TAG, "Safe closing websocket from main voice loop...");
        ws_voice_disconnect(g_ws);
      }
    }

    /* ── 1. 读麦克风 ── */
    esp_err_t r = voice_io_mic_read(false, buf, bytes_per_chunk);
    if (r != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    /* ── 物理防御隔离：当正在播放流媒体音乐或 TTS 时，强行挂起麦克风监听 ──
     * 1. 防止喇叭声音直接灌入麦克风导致死循环或错误打断 (无硬件 AEC 时的必杀技)
     * 2. 避免同时进行 HTTP 下载、重采样、I2S播放 和 本地唤醒/VAD 模型运算引发的极限过载
     */
    if (stream_player_is_playing()) {
      vTaskDelay(pdMS_TO_TICKS(50));
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
      if (xTaskGetTickCount() < g_wake_cooldown_end) {
        break;
      }
      wakenet_state_t wn = g_wakenet->detect(g_wn_data, audio);
      if (wn == WAKENET_DETECTED) {
        ESP_LOGI(TAG, "Wake word detected!");

        if (g_cb.on_wake)
          g_cb.on_wake(g_cb.ctx);

        /* 确保 WebSocket 连接可用（先销毁旧句柄再重连，避免幂等短路） */
        if (g_ws && !ws_voice_is_connected(g_ws)) {
          ws_voice_disconnect(g_ws);
          ws_voice_connect(g_ws);
        }

        /* 通知服务器: 唤醒 + 开始录音 */
        if (g_ws && ws_voice_is_connected(g_ws)) {
          char msg[128];
          snprintf(msg, sizeof(msg),
                   "{\"event\":\"wake_word_detected\",\"ts\":%lld}",
                   (long long)(esp_timer_get_time() / 1000));
          safe_send_text(msg, 1000);
          safe_send_text("{\"event\":\"recording_started\"}", 1000);
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
      static uint8_t s_batch[4096];
      static int s_batch_off = 0;

      /* 预判: 下一帧能否塞进录音缓冲区？不能 → 自动结束录音 */
      int chunk_s = bytes_per_chunk / (int)sizeof(int16_t);
      if (!g_ab.recording || (g_ab.rec_len + chunk_s > g_ab.rec_buf_samples)) {
        if (g_ab.rec_len > 0) {
          /* 先冲刷 s_batch 中未满 4096 字节的残留数据 */
          if (s_batch_off > 0 && g_ws && ws_voice_is_connected(g_ws)) {
            safe_send_binary(s_batch, s_batch_off, 1000);
            s_batch_off = 0;
          }
          audio_buf_record_stop(&g_ab);
          if (g_ws && ws_voice_is_connected(g_ws)) {
            safe_send_text("{\"event\":\"recording_ended\"}", 1000);
          }
          g_state = ST_WAITING_RESP;
          g_resp_start_tick = xTaskGetTickCount();
          audio_buf_resp_begin(&g_ab);
          ESP_LOGI(TAG, "Recording full, waiting for LLM response...");
        }
        break;
      }

      /* 录音缓冲区追加 */
      audio_buf_record_feed(&g_ab, audio, chunk_s);

      /*
       * 流式发送：每 4 帧（128ms）累积发送一次，降低帧率 75%。
       * Windows websockets 处理 32 帧/秒的二进制帧不够快，
       * TCP 接收缓冲区反压填满 ESP32 的 SND_BUF 导致 transport_poll_write
       * 超时。 累积发送保持 32KB/s 吞吐量不变，但帧率降到 8 帧/秒，
       * 给服务器事件循环足够的喘息空间。
       */
      {
        memcpy(s_batch + s_batch_off, (const uint8_t *)audio, bytes_per_chunk);
        s_batch_off += bytes_per_chunk;
        if (s_batch_off >= (int)sizeof(s_batch)) {
          s_batch_off = 0;
          if (g_ws && ws_voice_is_connected(g_ws)) {
            int sent = safe_send_binary(s_batch, sizeof(s_batch), 1000);
            if (sent < 0) {
              ESP_LOGW(TAG, "Binary send failed (%d), exiting dialogue", sent);
              s_batch_off = 0;
              audio_buf_record_stop(&g_ab);
              exit_dialogue();
              break;
            }
          }
        }
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
          /* 录音 <2s 时用更宽松的静音阈值（1.2s），避免长句子中途换气被截断 */
          int threshold = (audio_buf_record_duration(&g_ab) < 2.0f)
                              ? VA_SILENCE_FRAMES_EARLY
                              : VA_SILENCE_FRAMES;
          if (g_vad_silence_cnt >= threshold) {
            ESP_LOGI(TAG, "Silence, stopping (%.1fs, thresh=%dms)",
                     audio_buf_record_duration(&g_ab), threshold * 30);
            audio_buf_record_stop(&g_ab);
            g_streaming = false;

            size_t rec_len = 0;
            rec_len = g_ab.rec_len;

            /* 录音足够长（>250ms）→ 已经实时发送完毕，只需发结束标记 */
            if (g_user_spoke && rec_len > (size_t)(VA_SAMPLE_RATE / 4)) {
              /* 先冲刷 s_batch 中未满 4096 字节的残留数据 */
              if (s_batch_off > 0 && g_ws && ws_voice_is_connected(g_ws)) {
                safe_send_binary(s_batch, s_batch_off, 1000);
                s_batch_off = 0;
              }
              if (g_ws && ws_voice_is_connected(g_ws)) {
                safe_send_text("{\"event\":\"recording_ended\"}", 1000);
              }
              g_state = ST_WAITING_RESP;
              g_resp_start_tick = xTaskGetTickCount();
              audio_buf_resp_begin(&g_ab);
            } else {
              /* 录音太短 → 取消，重新开始录音 */
              s_batch_off = 0;
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

      /* WebSocket 保活：录音期间每 2s 发送一次心跳，防止 TCP 空闲断连 */
      if (g_ws && ws_voice_is_connected(g_ws)) {
        TickType_t now = xTaskGetTickCount();
        if ((now - g_last_ws_keepalive_tick) > pdMS_TO_TICKS(2000)) {
          ws_voice_send_text(g_ws, "{\"event\":\"heartbeat\"}", 500);
          g_last_ws_keepalive_tick = now;
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
      /* 30 秒超时：Python 端 ASR/LLM/TTS 任一环节失败都可能导致无响应 */
      if ((xTaskGetTickCount() - g_resp_start_tick) >
          pdMS_TO_TICKS(VA_RESP_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Response timeout (%ds), exiting",
                 VA_RESP_TIMEOUT_MS / 1000);
        exit_dialogue();
        break;
      }

      /* TTS 播放中不做打断检测（无硬件 AEC，VAD 无法区分喇叭声和人声） */

      if (g_ab.resp_played && !g_ab.streaming) {
        if (g_cb.on_response_done)
          g_cb.on_response_done(g_cb.ctx);

        if (g_is_web_simulated) {
            ESP_LOGI(TAG, "Web simulated text finished, back to wakeup...");
            safe_send_text("{\"event\":\"session_end\"}", 1000);
            exit_dialogue_ex(false);
        } else {
            /* TTS 播完 → 进入倾听模式，等待用户说下一句 */
            g_state = ST_WAITING_TURN;
            g_turn_start_tick = xTaskGetTickCount();
            if (g_vad)
              vad_reset_trigger(g_vad);
            ESP_LOGI(TAG, "Listening for next turn...");
        }
      }
      break;
    }

    /* ========================================================
     * ST_WAITING_TURN — 多轮对话：等待用户说下一句
     *
     * TTS 播放完毕后进入此状态，通过 VAD 检测用户是否说话。
     * - 检测到语音 → 自动进入录音（无需唤醒词）
     * - 超时 8 秒未说话 → 回到等待唤醒
     * ======================================================== */
    case ST_WAITING_TURN: {
      if ((xTaskGetTickCount() - g_turn_start_tick) >
          pdMS_TO_TICKS(VA_TURN_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "Turn timeout (%ds), back to wakeup",
                 VA_TURN_TIMEOUT_MS / 1000);
        safe_send_text("{\"event\":\"session_end\"}", 1000);
        exit_dialogue_ex(false);
        break;
      }

      /* 前 800ms 冷却期：跳过 VAD，防止喇叭回声误触发 */
      if ((xTaskGetTickCount() - g_turn_start_tick) <
          pdMS_TO_TICKS(VA_INTERRUPT_MIN_PLAY_MS)) {
        break;
      }

      /* 倾听中若 TTS/音乐仍在播放（喇叭出声）→ 跳过 VAD，
       * 避免喇叭回声被误认为用户说话 */
      if (g_vad && !g_ab.is_playing) {
        /* 先算 RMS 能量：连续 0 值或极小值表示"完全没声音"，
         * 直接跳过不进 VAD，减少不必要的计算 */
        int32_t sum = 0;
        const int frame_len = VA_SAMPLE_RATE * 30 / 1000;
        for (int i = 0; i < frame_len; i++)
          sum += (int32_t)audio[i] * audio[i];
        int32_t rms = (int32_t)sqrt((double)sum / frame_len);

        /* 实时 RMS：同一行刷新，不换行 */
        printf("\r[va] Listening RMS=%ld/%-6d", (long)rms,
               VA_TURN_RMS_THRESHOLD);
        fflush(stdout);

        /* 三层过滤（对齐 ST_RECORDING 的判断强度）：
         *   ① RMS < 100 → 太弱，麦克风底噪，完全没声音
         *   ② VAD 非语音 → 重置计数
         *   ③ 连续 5 帧（150ms）语音 → 触发录音 */
        if (rms < VA_TURN_RMS_THRESHOLD) {
          /* 层 ①：完全没声音，跳过 */
        } else {
          vad_state_t vs = vad_process(g_vad, audio, VA_SAMPLE_RATE, 30);
          static int turn_speech_cnt = 0;
          if (vs == VAD_SPEECH) {
            turn_speech_cnt++;
            if (turn_speech_cnt >= 5) {
              turn_speech_cnt = 0;
              /* 换行，避免后续 INFO 日志覆盖 RMS 行 */
              printf("\n");
              ESP_LOGI(TAG, "Turn speech detected, auto-recording (RMS=%d)",
                       rms);
              if (g_ws && ws_voice_is_connected(g_ws))
                safe_send_text("{\"event\":\"recording_started\"}", 1000);
              enter_recording("turn");
              break;
            }
          } else {
            turn_speech_cnt = 0;
          }
        }
      }
      break;
    }
    }

    /* ── 4. 强制延时礼让（非常关键！）──
     * 哪怕只延时 1 个 Tick (10ms)，也能强行把 CPU 让给系统空闲任务(IDLE)
     * 从而触发系统喂狗（清空 Task Watchdog 定时器），避免 100% 霸占导致重启！*/
    vTaskDelay(1);
  }

  free(buf);
}

/*
 * 外部强制唤醒（触摸屏按钮等）。
 * 仅在等待唤醒状态时生效，等同于检测到唤醒词。
 */
void va_force_wake(void) {
  // 💡 第一步：若设备正处于繁忙中，执行打断并无痛回滚状态
  if (g_state != ST_WAITING_WAKEUP) {
    ESP_LOGI(TAG, "Force wake interrupts busy state: %d", g_state);

    // 1. 强行停止本地音乐/测试音流播放
    stream_player_stop();

    // 2. 停止并释放下行 TTS 音频流缓冲区
    if (g_ab.streaming) {
      audio_buf_stream_finish(&g_ab);
    }

    // 3. 强行退回到等待唤醒态（传入 false 保持 WebSocket 长连接不掉线）
    exit_dialogue_ex(false);
  }

  // 💡 第二步：状态机已安全处于 WAITING_WAKEUP，快速开启录音
  if (g_state == ST_WAITING_WAKEUP) {
    if (g_cb.on_wake)
      g_cb.on_wake(g_cb.ctx);

    /* 确保 WebSocket 连接可用（先销毁旧句柄再重连，避免幂等短路） */
    if (g_ws && !ws_voice_is_connected(g_ws)) {
      ws_voice_disconnect(g_ws);
      ws_voice_connect(g_ws);
    }

    /* 通知服务器：重新开始一轮录音（这会向服务端触发 cancel_requested
     * 打断上一轮推流） */
    if (g_ws && ws_voice_is_connected(g_ws)) {
      safe_send_text("{\"event\":\"wake_word_detected\"}", 1000);
      safe_send_text("{\"event\":\"recording_started\"}", 1000);
    }

    enter_recording("force");
  }
}

/*
 * 查询语音助手是否忙碌。
 * UI 层用于暂停照片自动轮换。
 */
bool va_is_active(void) { return g_state != ST_WAITING_WAKEUP; }

esp_err_t voice_assistant_send_text(const char *text) {
  if (!text)
    return ESP_ERR_INVALID_ARG;
  return safe_send_text(text, 1000) >= 0 ? ESP_OK : ESP_FAIL;
}