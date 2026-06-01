/*
 * audio_buf.c — 音频缓冲区管理器实现
 *
 * 管理三种音频缓冲区:
 *   1. 录音缓冲区: 10 秒线性缓冲区，audio_buf_record_feed() 追加
 *   2. 响应缓冲区: 32 秒一次性缓冲区，audio_buf_resp_play() 整体播放
 *   3. 流式环形缓冲区: 32 KB，生产者（WS 回调）→ 消费者（I2S 播放）
 *
 * 环形缓冲区是流式低延迟播放的核心:
 *   - stream_write 指针在 WS_VOICE_DATA_BINARY 回调中前进（生产者）
 *   - stream_read 指针在 audio_buf_stream_feed() 中前进（消费者触发播放）
 *   - 当可读数据 ≥ 3200 字节（200ms）时自动调用 voice_io_spk_play_stream()
 *
 * 移植自 xiaozhi-replica audio_manager.cc。
 */

#include <string.h>
#include "audio_buf.h"
#include "voice_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_buf";

/* ── 生命周期 ───────────────────────────────────────────────── */

/*
 * 分配所有内部缓冲区并初始化状态。
 * 录音: 10s × 16000Hz × 2B = 320KB
 * 响应: 32s × 16000Hz × 2B = 1024KB
 * 流式: 32KB 环形
 * 总共约 1.4 MB，建议配合 PSRAM 使用。
 */
esp_err_t audio_buf_init(audio_buf_t *ab)
{
    if (!ab) return ESP_ERR_INVALID_ARG;

    ab->sample_rate      = 16000;
    ab->record_dur_sec   = 10;
    ab->response_dur_sec = 32;

    ab->rec_buf_samples  = ab->sample_rate * ab->record_dur_sec;
    ab->resp_buf_samples = ab->sample_rate * ab->response_dur_sec;

    /* 分配录音缓冲区 */
    ab->rec_buf = malloc(ab->rec_buf_samples * sizeof(int16_t));
    if (!ab->rec_buf) {
        ESP_LOGE(TAG, "rec buffer alloc fail (%zu bytes)", ab->rec_buf_samples * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }

    /* 分配响应缓冲区（calloc 保证初始为静音） */
    ab->resp_buf = calloc(ab->resp_buf_samples, sizeof(int16_t));
    if (!ab->resp_buf) {
        ESP_LOGE(TAG, "resp buffer alloc fail");
        free(ab->rec_buf); ab->rec_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 分配流式环形缓冲区 */
    ab->stream_buf = malloc(AUDIO_BUF_STREAMING_SIZE);
    if (!ab->stream_buf) {
        ESP_LOGE(TAG, "stream buffer alloc fail");
        free(ab->rec_buf);  ab->rec_buf  = NULL;
        free(ab->resp_buf); ab->resp_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 初始化状态 */
    ab->rec_len      = 0;
    ab->recording    = false;
    ab->resp_len     = 0;
    ab->resp_played  = false;
    ab->stream_write = 0;
    ab->stream_read  = 0;
    ab->streaming    = false;

    ESP_LOGI(TAG, "Initialised — rec=%zums resp=%zums stream=%dKB",
             ab->rec_buf_samples * sizeof(int16_t) / 1024,
             ab->resp_buf_samples * sizeof(int16_t) / 1024,
             AUDIO_BUF_STREAMING_SIZE / 1024);
    return ESP_OK;
}

/*
 * 释放所有内部缓冲区。可安全重复调用。
 */
void audio_buf_deinit(audio_buf_t *ab)
{
    if (!ab) return;
    if (ab->rec_buf)    { free(ab->rec_buf);    ab->rec_buf    = NULL; }
    if (ab->resp_buf)   { free(ab->resp_buf);   ab->resp_buf   = NULL; }
    if (ab->stream_buf) { free(ab->stream_buf); ab->stream_buf = NULL; }
}

/* ── 录音操作 ───────────────────────────────────────────────── */

void audio_buf_record_start(audio_buf_t *ab) { ab->recording = true; ab->rec_len = 0; }
void audio_buf_record_stop(audio_buf_t *ab)  { ab->recording = false; }

/*
 * 追加样本到录音缓冲区。满时返回 false 并打印告警。
 */
bool audio_buf_record_feed(audio_buf_t *ab, const int16_t *data, size_t samples)
{
    if (!ab->recording || !ab->rec_buf) return false;
    if (ab->rec_len + samples > ab->rec_buf_samples) {
        ESP_LOGW(TAG, "rec buffer full");
        return false;
    }
    memcpy(&ab->rec_buf[ab->rec_len], data, samples * sizeof(int16_t));
    ab->rec_len += samples;
    return true;
}

void audio_buf_record_clear(audio_buf_t *ab) { ab->rec_len = 0; }

/*
 * 返回已录制时长（秒）。
 */
float audio_buf_record_duration(const audio_buf_t *ab)
{
    return (float)ab->rec_len / (float)ab->sample_rate;
}

bool audio_buf_record_is_full(const audio_buf_t *ab)
{
    return ab->rec_len >= ab->rec_buf_samples;
}

/* ── 响应操作 ───────────────────────────────────────────────── */

void audio_buf_resp_begin(audio_buf_t *ab)
{
    ab->resp_len    = 0;
    ab->resp_played = false;
}

/*
 * 将 AI 响应音频数据拷贝到响应缓冲区。
 * 数据量不能超过缓冲区容量（32 秒）。
 */
bool audio_buf_resp_add(audio_buf_t *ab, const uint8_t *data, size_t bytes)
{
    size_t samples = bytes / sizeof(int16_t);
    if (samples > ab->resp_buf_samples) return false;
    memcpy(ab->resp_buf, data, bytes);
    ab->resp_len = samples;
    return true;
}

/*
 * 播放响应缓冲区中的音频。最多重试 3 次，每次失败后延时 100ms。
 */
esp_err_t audio_buf_resp_play(audio_buf_t *ab)
{
    if (ab->resp_len == 0) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 3 && ret != ESP_OK; i++) {
        ret = voice_io_spk_play((const uint8_t *)ab->resp_buf,
                                ab->resp_len * sizeof(int16_t));
        if (ret != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    }
    ab->resp_played = true;
    return ret;
}

/* ── 流式环形缓冲区操作 ─────────────────────────────────────── */

void audio_buf_stream_begin(audio_buf_t *ab)
{
    ab->streaming    = true;
    ab->stream_write = 0;
    ab->stream_read  = 0;
    if (ab->stream_buf) memset(ab->stream_buf, 0, AUDIO_BUF_STREAMING_SIZE);
}

/*
 * 计算环形缓冲区中可读数据量（字节）。
 * stream_write >= stream_read: 正常情况，数据未绕回
 * stream_write <  stream_read: 写指针已绕回，读指针尚未绕回
 */
static size_t stream_avail(const audio_buf_t *ab)
{
    if (ab->stream_write >= ab->stream_read)
        return ab->stream_write - ab->stream_read;
    return AUDIO_BUF_STREAMING_SIZE - ab->stream_read + ab->stream_write;
}

/*
 * 计算环形缓冲区剩余空间（字节）。
 * 保留 1 字节间隙以避免读写指针重合时无法区分空/满。
 */
static size_t stream_space(const audio_buf_t *ab)
{
    return AUDIO_BUF_STREAMING_SIZE - stream_avail(ab) - 1;
}

/*
 * 从环形缓冲区读取指定长度的数据到输出缓冲区。
 * 处理绕回情况: 先读尾部连续段，再读头部剩余段。
 */
static void stream_read_chunk(audio_buf_t *ab, uint8_t *out, size_t len)
{
    size_t to_end = AUDIO_BUF_STREAMING_SIZE - ab->stream_read;
    if (len <= to_end) {
        /* 不需要绕回 */
        memcpy(out, ab->stream_buf + ab->stream_read, len);
        ab->stream_read += len;
    } else {
        /* 需要绕回: 先读尾部，再读头部 */
        memcpy(out, ab->stream_buf + ab->stream_read, to_end);
        memcpy(out + to_end, ab->stream_buf, len - to_end);
        ab->stream_read = len - to_end;
    }
    if (ab->stream_read >= AUDIO_BUF_STREAMING_SIZE)
        ab->stream_read = 0;
}

/*
 * 向环形缓冲区推入音频数据（生产者: WebSocket 回调）。
 *
 * 设计要点:
 *   1. 写入数据到环形缓冲区
 *   2. 当累积 ≥ STREAMING_CHUNK (3200 字节 ≈ 200ms) 时，
 *      自动触发播放，边收边播实现低延迟
 *   3. 播放失败时停止消费，避免积压
 */
bool audio_buf_stream_feed(audio_buf_t *ab, const uint8_t *data, size_t len)
{
    if (!ab->streaming || !ab->stream_buf || !data) return false;
    if (len > stream_space(ab)) {
        ESP_LOGW(TAG, "stream buffer full");
        return false;
    }

    /* 写入数据（处理绕回） */
    size_t to_end = AUDIO_BUF_STREAMING_SIZE - ab->stream_write;
    if (len <= to_end) {
        memcpy(ab->stream_buf + ab->stream_write, data, len);
        ab->stream_write += len;
    } else {
        memcpy(ab->stream_buf + ab->stream_write, data, to_end);
        memcpy(ab->stream_buf, data + to_end, len - to_end);
        ab->stream_write = len - to_end;
    }
    if (ab->stream_write >= AUDIO_BUF_STREAMING_SIZE)
        ab->stream_write = 0;

    /* 每次满了 200ms 就播放一块 */
    while (stream_avail(ab) >= AUDIO_BUF_STREAMING_CHUNK) {
        uint8_t chunk[AUDIO_BUF_STREAMING_CHUNK];
        stream_read_chunk(ab, chunk, AUDIO_BUF_STREAMING_CHUNK);
        if (voice_io_spk_play_stream(chunk, AUDIO_BUF_STREAMING_CHUNK) != ESP_OK) break;
    }
    return true;
}

/*
 * 流式播放收尾: 播放环形缓冲区中剩余的不足一块的尾部数据，
 * 然后关闭流式状态。
 */
void audio_buf_stream_finish(audio_buf_t *ab)
{
    if (!ab->streaming) return;

    size_t remaining = stream_avail(ab);
    if (remaining > 0) {
        uint8_t *tail = malloc(remaining);
        if (tail) {
            stream_read_chunk(ab, tail, remaining);
            voice_io_spk_play(tail, remaining);  /* 最后一次性播放，play 内部会关功放 */
            free(tail);
        }
    }
    ab->streaming    = false;
    ab->stream_write = 0;
    ab->stream_read  = 0;
}
