/*
 * audio_buf.h — 音频缓冲区管理器
 *
 * 管理三种音频缓冲区，覆盖「录音 → LLM 响应 → 流式播放」全链路:
 *   1. 录音缓冲区   — 连续记录麦克风音频，最大 10 秒
 *   2. 响应缓冲区   — 存放完整的 AI 语音回复（一次性）
 *   3. 流式播放环形缓冲区 — 32 KB 环形队列，200ms 为一块边收边播
 *
 * 移植自 xiaozhi-replica audio_manager.h（C++ 类 → C 结构体 + 函数）。
 * 所有公开函数以 audio_buf_ 为前缀，第一个参数为 audio_buf_t 指针。
 */

#ifndef AUDIO_BUF_H
#define AUDIO_BUF_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 流式缓冲区常量 */
#define AUDIO_BUF_STREAMING_SIZE  32768   /* 环形缓冲区总大小 32 KB          */
#define AUDIO_BUF_STREAMING_CHUNK 3200    /* 每次播放块大小 ≈200ms @16kHz    */

/**
 * 音频缓冲区结构体（POD 类型，支持零初始化）。
 *
 * 字段分组:
 *   [参数]   sample_rate / record_dur_sec / response_dur_sec
 *   [录音]   rec_buf / rec_buf_samples / rec_len / recording
 *   [响应]   resp_buf / resp_buf_samples / resp_len / resp_played
 *   [流式]   stream_buf / stream_write / stream_read / streaming
 */
typedef struct {
    /* ── 参数 ── */
    uint32_t sample_rate;        /* 采样率，固定 16000 Hz              */
    uint32_t record_dur_sec;     /* 录音最大时长，默认 10 秒          */
    uint32_t response_dur_sec;   /* 响应最大时长，默认 32 秒          */

    /* ── 录音缓冲区（线性，非环形）── */
    int16_t *rec_buf;            /* 录音样本存储区                    */
    size_t   rec_buf_samples;    /* 缓冲区容量（样本数）              */
    size_t   rec_len;            /* 已录制样本数                      */
    bool     recording;          /* 是否正在录音                      */

    /* ── 响应缓冲区（一次性）── */
    int16_t *resp_buf;           /* 响应音频存储区                    */
    size_t   resp_buf_samples;   /* 缓冲区容量（样本数）              */
    size_t   resp_len;           /* 响应音频样本数                    */
    bool     resp_played;        /* 是否已播放完毕                    */

    /* ── 流式播放环形缓冲区 ── */
    uint8_t *stream_buf;         /* 32 KB 环形缓冲区                  */
    size_t   stream_write;       /* 写指针（生产者: WS 回调）         */
    size_t   stream_read;        /* 读指针（消费者: 播放线程）        */
    bool     streaming;          /* 是否处于流式播放状态              */
    bool     is_playing;         /* 是否正在播放音频（TTS/音乐）        */
} audio_buf_t;

/* ── 生命周期 ───────────────────────────────────────────────── */

/**
 * 分配并初始化所有内部缓冲区。
 * 录音缓冲区 = 10s × 16kHz × 2 字节 = 320 KB
 * 响应缓冲区 = 32s × 16kHz × 2 字节 ≈ 1 MB
 * 流式缓冲区 = 32 KB
 *
 * @param ab [in/out] 指向调用者提供的 audio_buf_t 实例
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 内存不足
 */
esp_err_t audio_buf_init(audio_buf_t *ab);

/**
 * 释放所有内部缓冲区。可安全重复调用。
 *
 * @param ab [in/out] 指向已初始化的 audio_buf_t 实例
 */
void audio_buf_deinit(audio_buf_t *ab);

/* ── 录音操作 ───────────────────────────────────────────────── */

/**
 * 开始录音。重置 rec_len，设置 recording = true。
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_record_start(audio_buf_t *ab);

/**
 * 停止录音。设置 recording = false，不释放数据。
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_record_stop(audio_buf_t *ab);

/**
 * 向录音缓冲区追加样本数据。
 *
 * @param ab      [in/out] 缓冲区实例
 * @param data    样本数据（int16_t 数组）
 * @param samples 样本数量（非字节数）
 * @return true 写入成功，false 缓冲区已满或未在录音状态
 */
bool audio_buf_record_feed(audio_buf_t *ab, const int16_t *data, size_t samples);

/**
 * 清空录音计数（不释放内存）。
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_record_clear(audio_buf_t *ab);

/**
 * 获取已录制的音频时长。
 * @param ab 缓冲区实例
 * @return 时长（秒），浮点数
 */
float audio_buf_record_duration(const audio_buf_t *ab);

/**
 * 检查录音缓冲区是否已满。
 * @param ab 缓冲区实例
 * @return true 已满
 */
bool  audio_buf_record_is_full(const audio_buf_t *ab);

/* ── 响应操作（一次性）─────────────────────────────────────── */

/**
 * 初始化响应缓冲区状态（播放前调用）。
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_resp_begin(audio_buf_t *ab);

/**
 * 将完整的响应音频数据拷贝到响应缓冲区。
 *
 * @param ab    [in/out] 缓冲区实例
 * @param data  音频数据（字节流）
 * @param bytes 字节长度
 * @return true 拷贝成功，false 数据超出容量
 */
bool audio_buf_resp_add(audio_buf_t *ab, const uint8_t *data, size_t bytes);

/**
 * 通过扬声器播放响应缓冲区中的音频（阻塞，最多重试 3 次）。
 *
 * @param ab [in/out] 缓冲区实例
 * @return ESP_OK 播放成功
 */
esp_err_t audio_buf_resp_play(audio_buf_t *ab);

/* ── 流式播放操作 ───────────────────────────────────────────── */

/**
 * 开始流式播放会话。重置环形缓冲区读写指针。
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_stream_begin(audio_buf_t *ab);

/**
 * 向环形缓冲区推入音频数据。当累积数据 ≥ STREAMING_CHUNK 时
 * 自动触发播放（调用 voice_io_spk_play_stream）。
 *
 * @param ab   [in/out] 缓冲区实例
 * @param data 音频数据（字节流）
 * @param len  字节长度
 * @return true 推送成功，false 缓冲区满或未在流式状态
 */
bool audio_buf_stream_feed(audio_buf_t *ab, const uint8_t *data, size_t len);

/**
 * 播放环形缓冲区中剩余的所有数据，然后停止流式会话。
 * 在 LLM 响应结束时由 WebSocket 事件触发。
 *
 * @param ab [in/out] 缓冲区实例
 */
void audio_buf_stream_finish(audio_buf_t *ab);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_BUF_H */