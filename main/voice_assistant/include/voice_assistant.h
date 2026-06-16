/*
 * voice_assistant.h — 语音助手状态机
 *
 * 完整语音对话流水线:
 *   1. WakeNet9  唤醒词检测（"你好小智"） → 进入录音态
 *   2. VADNet1   语音活动检测 + 实时音频流发送
 *   3. WebSocket 连接 Python 服务器 → 本地 whisper + Ollama + edge-tts
 *   4. LLM 音频流返回 → 扬声器播放 → 回到录音态（连续对话）
 *
 * 同时支持 MultiNet7 本地命令词识别（照片浏览 + 雾化控制），
 * 无需云端即可响应。通过回调与 UI / 外设模块解耦。
 *
 * 运行在独立 FreeRTOS 任务中，通过 va_config_t 配置，
 * 通过 va_callbacks_t 向主程序通知事件。
 */

#ifndef VOICE_ASSISTANT_H
#define VOICE_ASSISTANT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── MultiNet7 命令 ID ──────────────────────────────────────── */

enum {
    VA_CMD_NEXT_PHOTO   = 200,  /* 下一张照片                        */
    VA_CMD_PREV_PHOTO   = 201,  /* 上一张照片                        */
    VA_CMD_MIST_ON      = 210,  /* 打开雾化加湿器（3 路全开）        */
    VA_CMD_MIST_OFF     = 211,  /* 关闭雾化加湿器                    */
    VA_CMD_BYE          = 220,  /* 拜拜，退出连续对话模式            */

    VA_CMD_VOL_UP       = 230,  /* 大声一点                          */
    VA_CMD_VOL_DOWN     = 231,  /* 小声一点                          */
    VA_CMD_BRIGHT_UP    = 240,  /* 调亮一点                          */
    VA_CMD_BRIGHT_DOWN  = 241,  /* 调暗一点                          */
    VA_CMD_MIST_LEVEL_UP   = 250,  /* 加大喷雾                       */
    VA_CMD_MIST_LEVEL_DOWN = 251,  /* 减小喷雾                       */
    VA_CMD_AUDIO_PLAY   = 260,  /* 播放音乐                          */
    VA_CMD_AUDIO_STOP   = 261,  /* 停止播放/关掉声音                 */
    VA_CMD_MODE_SLEEP   = 270,  /* 睡觉/休眠模式                     */
};

/* ── 回调函数集 ─────────────────────────────────────────────── */

/**
 * 语音助手回调结构体。
 * 所有回调从 voice task 的上下文中被调用，应尽量短小，
 * 耗时操作应委托给其他任务。
 */
typedef struct {
    /**
     * 本地命令识别回调。
     * @param cmd_id 命令 ID（VA_CMD_* 枚举值）
     * @param ctx    用户上下文指针
     */
    void (*on_command)(int cmd_id, void *ctx);

    /**
     * 唤醒词检测回调（可用于 LED 闪烁 / UI 图标动画）。
     * @param ctx 用户上下文指针
     */
    void (*on_wake)(void *ctx);

    /**
     * LLM 语音回复播放完毕回调。
     * @param ctx 用户上下文指针
     */
    void (*on_response_done)(void *ctx);

    /**
     * 状态机状态变化回调。
     * @param state 新状态: 0=等待唤醒, 1=录音中, 2=等待 LLM 回复
     * @param ctx   用户上下文指针
     */
    void (*on_state_change)(int state, void *ctx);

    /** 用户上下文指针，原样透传给每个回调。 */
    void *ctx;
} va_callbacks_t;

/* ── 配置 ──────────────────────────────────────────────────── */

typedef struct {
    const char *ws_uri;          /* LLM WebSocket 服务器地址            */
    const char *wake_word;       /* 唤醒词，"nihaoxiaozhi" 或 NULL=默认 */
    int         det_mode;        /* 唤醒检测灵敏度 80/90/95，默认 90    */
} va_config_t;

/* ── 公开 API ───────────────────────────────────────────────── */

/**
 * @brief 预加载 ESP-SR 语音模型
 * 
 * 应在系统启动的最早期调用，以防止内存碎片化导致分配连续大块 PSRAM 失败。
 *
 * @param det_mode 唤醒灵敏度 (推荐 90)
 * @return esp_err_t ESP_OK 成功，ESP_FAIL 失败
 */
esp_err_t va_preload_models(int det_mode);

/**
 * @brief 初始化语音助手模块
。
 * 依次完成: 等待 Wi-Fi → 初始化 I2S 音频 → 分配音频缓冲区 →
 *         连接 WebSocket → 加载 ESP-SR 模型。
 * 阻塞直到 Wi-Fi 就绪或超时（60 秒）。
 *
 * @param cfg 配置参数（ws_uri 必填）
 * @param cbs 回调函数集（可为 NULL，不接收事件）
 * @return ESP_OK 初始化成功
 */
esp_err_t va_init(const va_config_t *cfg, const va_callbacks_t *cbs);

/**
 * 语音助手主循环 — 永不返回。
 * 每帧循环: 读麦克风 → 噪声抑制 → WakeNet / VAD / MultiNet 处理 →
 *          状态机驱动 → WebSocket 收发。
 * 调用者应将其放在独立 FreeRTOS 任务中。
 */
void va_run(void);

/**
 * 外部强制唤醒（例如触摸屏按钮触发）。
 * 仅在 ST_WAITING_WAKEUP 状态时生效，效果等同于检测到唤醒词。
 */
void va_force_wake(void);

/**
 * 查询语音助手是否处于活跃状态（非等待唤醒）。
 * UI 侧可用于暂停照片自动轮换，避免与语音交互冲突。
 *
 * @return true 正在录音或等待 LLM 回复
 */
bool va_is_active(void);

/**
 * @brief 发送文本消息到语音助手 WebSocket 连接中（线程安全）
 */
esp_err_t voice_assistant_send_text(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_ASSISTANT_H */
