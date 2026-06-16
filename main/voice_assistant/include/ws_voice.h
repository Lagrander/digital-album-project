/*
 * ws_voice.h — LLM 语音对话 WebSocket 客户端
 *
 * 对 esp_websocket_client 的纯 C 封装，提供:
 *   - 自动重连（5 秒间隔）
 *   - 事件回调（函数指针 + 用户上下文）
 *   - 文本 / 二进制帧发送
 *
 * 移植自 xiaozhi-replica websocket_client.h（C++ std::function → C 函数指针）。
 * 线程安全: 所有公开函数可从任意 FreeRTOS 任务调用。
 */

#ifndef WS_VOICE_H
#define WS_VOICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 事件类型枚举 ──────────────────────────────────────────── */

/** WebSocket 事件类型 */
typedef enum {
    WS_VOICE_CONNECTED,       /* 连接建立成功                      */
    WS_VOICE_DISCONNECTED,    /* 连接断开（含主动断开和网络异常）  */
    WS_VOICE_DATA_TEXT,       /* 收到文本帧（JSON 命令/事件）      */
    WS_VOICE_DATA_BINARY,     /* 收到二进制帧（音频数据）          */
    WS_VOICE_ERROR,           /* 连接错误                          */
} ws_voice_event_t;

/** WebSocket 事件载荷 */
typedef struct {
    ws_voice_event_t type;    /* 事件类型                          */
    const uint8_t   *data;    /* 数据指针（文本帧或二进制帧）      */
    size_t           data_len;/* 数据长度（字节）                  */
    size_t           payload_offset; /* 当前数据在整个 payload 的偏移 */
    size_t           payload_len;    /* 整个 payload 的总长度 */
} ws_voice_evt_t;

/**
 * 事件回调函数签名。
 * 从 esp_websocket_client 内部任务中调用，应尽快返回，
 * 耗时操作应放入队列或委托给其他任务。
 *
 * @param evt      事件载荷
 * @param user_ctx 用户上下文（创建时传入）
 */
typedef void (*ws_voice_cb_t)(const ws_voice_evt_t *evt, void *user_ctx);

/* ── 不透明句柄 ────────────────────────────────────────────── */

/** WebSocket 客户端实例（不透明类型，字段定义在 .c 中） */
typedef struct ws_voice ws_voice_t;

/* ── API ────────────────────────────────────────────────────── */

/**
 * 创建 WebSocket 客户端实例（不连接）。
 *
 * @param uri            服务器地址，例如 "ws://192.168.1.100:8888"
 * @param auto_reconnect true=断线自动重连
 * @param cb             事件回调函数（可为 NULL）
 * @param user_ctx       用户上下文，透传给回调
 * @return 新实例指针，内存不足时返回 NULL
 */
ws_voice_t *ws_voice_create(const char *uri, bool auto_reconnect,
                            ws_voice_cb_t cb, void *user_ctx);

/**
 * 发起 WebSocket 连接。
 * 如果已连接则直接返回 ESP_OK。
 * 如果配置了 auto_reconnect，同时创建重连监视任务（5 秒间隔）。
 *
 * @param ws 客户端实例
 * @return ESP_OK 连接成功，ESP_FAIL 初始化失败
 */
esp_err_t ws_voice_connect(ws_voice_t *ws);

/**
 * 断开连接并释放内部句柄，停止重连任务。
 * 可安全重复调用。
 *
 * @param ws 客户端实例
 */
void      ws_voice_disconnect(ws_voice_t *ws);

/**
 * 查询当前连接状态。
 *
 * @param ws 客户端实例
 * @return true 已连接
 */
bool      ws_voice_is_connected(const ws_voice_t *ws);

/**
 * 发送文本帧（JSON 控制消息）。
 *
 * @param ws         客户端实例
 * @param text       文本内容（以 '\0' 结尾的 C 字符串）
 * @param timeout_ms 发送超时（毫秒）
 * @return ≥0 发送字节数，-1 未连接或发送失败
 */
int  ws_voice_send_text(ws_voice_t *ws, const char *text, int timeout_ms);

/**
 * 发送二进制帧（音频数据）。
 *
 * @param ws         客户端实例
 * @param data       二进制数据
 * @param len        数据长度（字节）
 * @param timeout_ms 发送超时（毫秒）
 * @return ≥0 发送字节数，-1 未连接或发送失败
 */
int  ws_voice_send_binary(ws_voice_t *ws, const uint8_t *data, size_t len, int timeout_ms);

/**
 * 销毁客户端实例。先断开连接，再释放内存。
 *
 * @param ws 客户端实例
 */
void ws_voice_destroy(ws_voice_t *ws);

#ifdef __cplusplus
}
#endif

#endif /* WS_VOICE_H */
