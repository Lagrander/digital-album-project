/**
 * @file ws_voice.c
 * @brief 全双工流式 WebSocket 音频通信链路
 *
 * @architecture
 * 构建与 Python 后端（voice_server.py）的通信：
 * 1. 上行 (Uplink)：持续发送 16kHz/16bit PCM 裸流。
 * 2. 下行 (Downlink)：接收服务端分块推送的 TTS PCM 数据，并存入缓冲供给播放器消费。
 * 3. 状态交互：处理 LLM 实时下发的 JSON 控制帧，实现极低延迟的全双工打断。
 */
/*
 * ws_voice.c — LLM 语音对话 WebSocket 客户端实现
 *
 * 对 esp_websocket_client 的纯 C 封装。
 *
 * 功能要点:
 *   - evt_handler(): 将 ESP WebSocket 事件转换为统一的 ws_voice_evt_t
 *   - recon_task_fn(): 独立 FreeRTOS 任务，每 5 秒检查连接状态并自动重连
 *   - 文本帧发送（JSON 控制消息）/ 二进制帧发送（PCM 音频数据）
 *
 * 移植自 xiaozhi-replica websocket_client.cc。
 * C++ → C 的关键变化:
 *   - std::function<EventCallback> → ws_voice_cb_t 函数指针 + void *user_ctx
 *   - std::string uri → char uri[256] 固定大小
 *   - 类 RAII 析构 → ws_voice_destroy() 显式调用
 */

#include "ws_voice.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ws_voice";

/* 内部常量 
 * 注意：由于 sdkconfig 中 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384，
 * 为了强制将 WebSocket 任务栈和接收缓冲分配在 PSRAM 中，以节省极度紧张的内部 RAM，
 * 此处必须将大小设置得大于 16KB。
 */
#define WS_BUF_SIZE 17000    /* WebSocket 接收缓冲区大小 (大于16KB分配至PSRAM) */
#define WS_TASK_STACK 17000  /* WebSocket 内部任务栈大小 (大于16KB分配至PSRAM) */
#define WS_RECON_STACK 2048  /* 重连监视任务栈大小              */

/*
 * WebSocket 客户端实例（不透明类型）。
 */
struct ws_voice {
  esp_websocket_client_handle_t handle; /* ESP WebSocket 客户端句柄 */
  char uri[256];                        /* 服务器 URI（固定 255 字符） */
  bool auto_reconnect;                  /* 是否自动重连              */
  bool connected;                       /* 当前连接状态              */
  ws_voice_cb_t callback;               /* 事件回调函数指针          */
  void *user_ctx;                       /* 用户上下文，透传给回调    */
  ws_voice_event_t last_data_type;      /* 上一次接收到的非 continuation 数据帧类型 */
};

/* ── 内部事件转发 ───────────────────────────────────────────── */

/*
 * ESP WebSocket 事件 → ws_voice_evt_t 统一事件转换。
 *
 * 从 esp_websocket_client 的内部任务中回调，应尽快返回。
 * 对二进制帧判断 op_code: 0x01=文本, 0x02=二进制。
 */
static void evt_handler(void *arg, esp_event_base_t base, int32_t event_id,
                        void *event_data) {
  ws_voice_t *ws = (ws_voice_t *)arg;
  esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
  if (!ws || !ws->callback)
    return;

  ws_voice_evt_t evt;
  memset(&evt, 0, sizeof(evt));

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ws->connected = true;
    evt.type = WS_VOICE_CONNECTED;
    ESP_LOGI(TAG, "Connected");
    break;
  case WEBSOCKET_EVENT_DISCONNECTED:
    ws->connected = false;
    evt.type = WS_VOICE_DISCONNECTED;
    ESP_LOGI(TAG, "Disconnected");
    break;
  case WEBSOCKET_EVENT_DATA:
    evt.data = (const uint8_t *)d->data_ptr;
    evt.data_len = d->data_len;
    evt.payload_offset = d->payload_offset;
    evt.payload_len = d->payload_len;
    if (d->op_code == 0x01) {
      ws->last_data_type = WS_VOICE_DATA_TEXT;
      evt.type = WS_VOICE_DATA_TEXT;
    } else if (d->op_code == 0x02) {
      ws->last_data_type = WS_VOICE_DATA_BINARY;
      evt.type = WS_VOICE_DATA_BINARY;
    } else if (d->op_code == 0x00) {
      evt.type = ws->last_data_type;
    } else {
      evt.type = WS_VOICE_DATA_BINARY;
    }
    break;
  case WEBSOCKET_EVENT_ERROR:
    ws->connected = false;
    evt.type = WS_VOICE_ERROR;
    break;
  default:
    return;
  }
  ws->callback(&evt, ws->user_ctx);
}

/* 删除了旧的 recon_task_fn，交由底层 esp_websocket_client 自动重连 */

/* ── 公开 API ───────────────────────────────────────────────── */

/*
 * 创建 WebSocket 客户端实例。
 * 仅分配内存 + 记录参数，不发起连接。连接需调用 ws_voice_connect()。
 */
ws_voice_t *ws_voice_create(const char *uri, bool auto_reconnect,
                            ws_voice_cb_t cb, void *user_ctx) {
  ws_voice_t *ws = calloc(1, sizeof(ws_voice_t));
  if (!ws)
    return NULL;
  strncpy(ws->uri, uri, sizeof(ws->uri) - 1);
  ws->auto_reconnect = auto_reconnect;
  ws->callback = cb;
  ws->user_ctx = user_ctx;
  return ws;
}

/*
 * 发起 WebSocket 连接。
 *
 * 配置: 8 KB 接收缓冲，10 秒网络超时。
 * auto_reconnect=true 时同时启动 5 秒间隔的重连监视任务。
 */
esp_err_t ws_voice_connect(ws_voice_t *ws) {
  if (!ws)
    return ESP_ERR_INVALID_ARG;
  if (ws->handle)
    return ESP_OK; /* 已连接，幂等 */

  esp_websocket_client_config_t cfg = {
      .uri = ws->uri,
      .buffer_size = WS_BUF_SIZE,
      .task_stack = WS_TASK_STACK,
      .reconnect_timeout_ms = 5000,   /* 重连间隔 5s，给 TIME_WAIT 释放时间 */
      .network_timeout_ms = 10000,     /* 网络超时 10s，给握手更多时间 */
      .ping_interval_sec = 10,         /* PING 降低频率，减少与音频发送的碰撞     */
      .pingpong_timeout_sec = 30,      /* PONG 超时放宽，容忍 TCP 缓冲区拥塞      */
  };

  ws->handle = esp_websocket_client_init(&cfg);
  if (!ws->handle) {
    ESP_LOGE(TAG, "Init failed");
    return ESP_FAIL;
  }

  /* 注册事件回调 */
  esp_websocket_register_events(ws->handle, WEBSOCKET_EVENT_ANY, evt_handler,
                                ws);

  esp_err_t ret = esp_websocket_client_start(ws->handle);
  if (ret != ESP_OK) {
    esp_websocket_client_destroy(ws->handle);
    ws->handle = NULL;
    return ret;
  }

  /* 启动自动重连监视任务 - 废除，依赖底层 reconnect_timeout_ms */
  return ESP_OK;
}

/*
 * 断开连接。
 * 流程: 停止重连任务 → 停止 WS 客户端 → 销毁句柄 → 标记断开。
 */
void ws_voice_disconnect(ws_voice_t *ws) {
  if (!ws)
    return;
  if (ws->handle) {
    esp_websocket_client_stop(ws->handle);
    esp_websocket_client_destroy(ws->handle);
    ws->handle = NULL;
  }
  ws->connected = false;
}

bool ws_voice_is_connected(const ws_voice_t *ws) {
  if (!ws || !ws->handle) return false;
  // 双重校验：软件标志与原生驱动物理连接状态共同核对，避开状态机死锁导致盲目发送数据
  return ws->connected && esp_websocket_client_is_connected(ws->handle);
}

/*
 * 发送文本帧。
 * timeout_ms 会转换为 FreeRTOS ticks。返回 -1 表示未连接。
 */
int ws_voice_send_text(ws_voice_t *ws, const char *text, int timeout_ms) {
  if (!ws || !ws->handle || !ws_voice_is_connected(ws))
    return -1;
  return esp_websocket_client_send_text(ws->handle, text, strlen(text),
                                        timeout_ms / portTICK_PERIOD_MS);
}

/*
 * 发送二进制帧（PCM 音频数据）。
 * timeout_ms 会转换为 FreeRTOS ticks。返回 -1 表示未连接。
 */
int ws_voice_send_binary(ws_voice_t *ws, const uint8_t *data, size_t len,
                         int timeout_ms) {
  if (!ws || !ws->handle || !ws_voice_is_connected(ws))
    return -1;
  return esp_websocket_client_send_bin(ws->handle, (const char *)data, len,
                                       timeout_ms / portTICK_PERIOD_MS);
}

/*
 * 销毁客户端实例。先断开连接，再释放内存。
 */
void ws_voice_destroy(ws_voice_t *ws) {
  if (!ws)
    return;
  ws_voice_disconnect(ws);
  free(ws);
}