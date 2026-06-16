/*
 * voice_io.h — I2S 音频输入输出驱动
 *
 * 驱动 INMP441 数字 MEMS 麦克风（I2S 输入）和 MAX98357A D 类功放（I2S 输出）。
 * 使用 ESP-IDF 5.x 标准模式 I2S 驱动，GPIO 引脚通过 Kconfig 配置。
 *
 * 硬件拓扑:
 *   INMP441 → I2S_NUM_0 (RX) → ESP32-S3
 *   ESP32-S3 → I2S_NUM_0 (TX) → MAX98357A → 扬声器
 *
 * 依赖: driver/i2s_std.h, driver/gpio.h
 */

#ifndef VOICE_IO_H
#define VOICE_IO_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 麦克风 I2S 引脚 ──────────────────────────────────────────
 * 引脚由 Kconfig 注入，所选 GPIO 均与 LCD RGB 接口无冲突。
 * - WS  (LRCLK): 左右声道时钟，INMP441 仅支持 I2S 飞利浦标准
 * - SCK (BCLK) : 位时钟
 * - SD  (DATA) : 串行数据输出
 */
#define VA_MIC_WS_PIN   CONFIG_VA_MIC_WS_PIN
#define VA_MIC_SCK_PIN  CONFIG_VA_MIC_SCK_PIN
#define VA_MIC_SD_PIN   CONFIG_VA_MIC_SD_PIN

/* ── 扬声器 I2S 引脚 ──────────────────────────────────────────
 * 引脚由 Kconfig 注入
 * SD_PIN: -1 表示无 GPIO 控制，硬件接地常开
 */
#define VA_SPK_BCLK_PIN CONFIG_VA_SPK_BCLK_PIN
#define VA_SPK_LRCK_PIN CONFIG_VA_SPK_LRCK_PIN
#define VA_SPK_DIN_PIN  CONFIG_VA_SPK_DIN_PIN
#define VA_SPK_SD_PIN   CONFIG_VA_SPK_SD_PIN

/* ── 麦克风 API ─────────────────────────────────────────────── */

/**
 * 初始化 INMP441 麦克风 I2S 接收通道。
 *
 * @param sample_rate     采样率（Hz），语音助手固定为 16000
 * @param channel         声道数，1 = 单声道
 * @param bits_per_sample 位深，16 或 32，INMP441 原生 24 位
 * @return ESP_OK 成功，否则返回 I2S 错误码
 *
 * 副作用: 丢弃前 3 次读取数据以清除上电噪声
 */
esp_err_t voice_io_mic_init(uint32_t sample_rate, int channel,
                            int bits_per_sample);

/**
 * 从麦克风 I2S 通道读取一帧音频数据（阻塞调用）。
 *
 * @param is_raw true=保留 32 位原始数据，false=限幅到 16 位
 * @param buffer [out] 音频样本缓冲区，调用者分配
 * @param len    缓冲区字节长度
 * @return ESP_OK 成功，否则 ESP_FAIL
 *
 * 注意: INMP441 输出 24 位数据左对齐在 32 位槽中，
 *       is_raw=false 时会限幅到 [-32768, 32767] 范围。
 */
esp_err_t voice_io_mic_read(bool is_raw, int16_t *buffer, int len);

/**
 * 返回已配置的麦克风声道数（固定返回 1）。
 */
int voice_io_mic_channel(void);

/**
 * 清空麦克风接收缓冲池中的陈旧积压音频数据。
 */
void voice_io_mic_clear(void);

/* ── 扬声器 API ─────────────────────────────────────────────── */

/**
 * 初始化 MAX98357A 扬声器 I2S 发送通道。
 * 配置 SD_MODE 引脚为推挽输出并拉高（功放使能）。
 *
 * @param sample_rate     采样率（Hz），与麦克风保持一致 16000
 * @param channel         声道数
 * @param bits_per_sample 位深
 * @return ESP_OK 成功
 */
esp_err_t voice_io_spk_init(uint32_t sample_rate, int channel,
                            int bits_per_sample);

/**
 * 播放完整音频缓冲区，播放完毕后自动停止 I2S 并关断功放。
 * 适用场景: 一次性播放短音频（如提示音、问候语）。
 *
 * @param data 音频数据（int16_t 字节流）
 * @param len  字节长度
 * @return ESP_OK 成功
 */
esp_err_t voice_io_spk_play(const uint8_t *data, size_t len);

/**
 * 流式播放音频数据，不停止 I2S 通道。
 * 适用场景: LLM 实时音频流，连续多次调用。
 * 调用者需要在流结束时调用 voice_io_spk_stop()。
 *
 * @param data 音频数据
 * @param len  字节长度
 * @return ESP_OK 成功
 */
esp_err_t voice_io_spk_play_stream(const uint8_t *data, size_t len);

/**
 * 发送静音帧、关断功放、停止 I2S 发送通道。
 * 在流式播放结束后调用，避免扬声器发出噪声。
 *
 * @return ESP_OK 成功
 */
esp_err_t voice_io_spk_stop(void);

/**
 * 强置硬件复位功放芯片（防抖与防 PLL 锁死）
 */
void voice_io_spk_force_reset(void);

/**
 * 硬件级全局音量设置 (0 ~ 100)
 */
void voice_io_set_spk_volume(uint8_t vol);
uint8_t voice_io_get_spk_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_IO_H */
