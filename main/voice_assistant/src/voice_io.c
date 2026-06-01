/*
 * voice_io.c — I2S 音频驱动实现
 *
 * INMP441 麦克风 (I2S_NUM_0 RX) + MAX98357A 扬声器 (I2S_NUM_1 TX)。
 * 使用 ESP-IDF 5.x 标准模式 I2S API，GPIO 引脚来自 Kconfig。
 *
 * 移植自 xiaozhi-replica bsp_board.cc（C++ 类成员 → C 静态文件变量）。
 *
 * 关键设计:
 *   - 麦克风: 飞利浦标准 I2S，24 位数据左对齐在 32 位槽 → 读后限幅到 16 位
 *   - 扬声器: MAX98357A SD_MODE 引脚控制功放开关，静音时关断省电
 *   - 流式播放: voice_io_spk_play_stream() 保持 I2S 通道开着连续写入
 *   - 一次性播放: voice_io_spk_play() 写完后自动停 I2S + 关功放
 */

#include <string.h>
#include "voice_io.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice_io";

/* ── 内部状态（文件级全局，单实例）────────────────────────── */

static i2s_chan_handle_t rx_chan;   /* 麦克风 I2S 接收通道句柄       */
static i2s_chan_handle_t tx_chan;   /* 扬声器 I2S 发送通道句柄       */
static bool tx_enabled;             /* 发送通道是否已使能            */

#define MIC_I2S_NUM  I2S_NUM_0      /* 麦克风使用 I2S 外设 0         */
#define SPK_I2S_NUM  I2S_NUM_1      /* 扬声器使用 I2S 外设 1         */

/* ── 麦克风实现 ─────────────────────────────────────────────── */

/*
 * 初始化 INMP441 麦克风 I2S 接收通道。
 *
 * 流程:
 *   1. 创建 I2S 通道（ESP32-S3 为主机，INMP441 为从机）
 *   2. 配置飞利浦标准时序 + 单声道 + 左声道槽位
 *   3. 使能通道后丢弃前 3 次读数（上电毛刺）
 */
esp_err_t voice_io_mic_init(uint32_t sample_rate, int channel, int bits_per_sample)
{
    esp_err_t ret;

    /* 创建 I2S RX 通道 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX channel create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 位宽: 16 或 32 */
    i2s_data_bit_width_t bw = (bits_per_sample == 32)
        ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    /* 配置飞利浦标准模式 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bw, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_VA_MIC_SCK_PIN,
            .ws   = CONFIG_VA_MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_VA_MIC_SD_PIN,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  /* INMP441 仅在左声道输出数据 */

    ret = i2s_channel_init_std_mode(rx_chan, &std_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RX std init fail"); return ret; }

    ret = i2s_channel_enable(rx_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RX enable fail"); return ret; }

    /* 丢弃上电初期的无效采样 */
    uint8_t *discard = malloc(8192);
    if (discard) {
        size_t n;
        for (int i = 0; i < 3; i++) {
            i2s_channel_read(rx_chan, discard, 8192, &n, pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        free(discard);
    }

    ESP_LOGI(TAG, "Mic I2S RX ready  rate=%lu ch=%d bits=%d", sample_rate, channel, bits_per_sample);
    return ESP_OK;
}

/*
 * 从麦克风读取一帧音频数据（阻塞）。
 *
 * INMP441 输出 24 位数据，左对齐在 32 位 I2S 槽位中。
 * is_raw=false 时对每个样本限幅到 int16_t 范围 [-32768, 32767]。
 * WakeNet / MultiNet 可直接使用限幅后的 16 位数据。
 */
esp_err_t voice_io_mic_read(bool is_raw, int16_t *buffer, int len)
{
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_chan, buffer, len, &bytes_read, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read fail: %s", esp_err_to_name(ret));
        return ret;
    }
    if (bytes_read != (size_t)len) {
        ESP_LOGW(TAG, "Partial read: %d / %d", (int)bytes_read, len);
    }
    if (!is_raw) {
        /* 24 位 → 16 位限幅 */
        int samples = len / (int)sizeof(int16_t);
        for (int i = 0; i < samples; i++) {
            int32_t s = (int32_t)buffer[i];
            if (s > 32767)  s = 32767;
            if (s < -32768) s = -32768;
            buffer[i] = (int16_t)s;
        }
    }
    return ESP_OK;
}

/*
 * 返回麦克风声道数。INMP441 固定单声道输出。
 */
int voice_io_mic_channel(void) { return 1; }

/* ── 扬声器实现 ─────────────────────────────────────────────── */

/*
 * 初始化 MAX98357A 扬声器 I2S 发送通道。
 *
 * 流程:
 *   1. 配置 SD_MODE 引脚为推挽输出，拉高使能功放
 *   2. 创建 I2S TX 通道
 *   3. 配置飞利浦标准时序 + 单/双声道
 *   4. 使能通道，设为 tx_enabled = true
 */
esp_err_t voice_io_spk_init(uint32_t sample_rate, int channel, int bits_per_sample)
{
    esp_err_t ret;

    /* MAX98357A SD_MODE: 高电平 = 工作，低电平 = 关断 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_VA_SPK_SD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(CONFIG_VA_SPK_SD_PIN, 1);

    /* 创建 I2S TX 通道 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SPK_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "TX chan fail"); return ret; }

    i2s_data_bit_width_t bw = (bits_per_sample == 32)
        ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    i2s_slot_mode_t sm = (channel == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bw, sm),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_VA_SPK_BCLK_PIN,
            .ws   = CONFIG_VA_SPK_LRCK_PIN,
            .dout = CONFIG_VA_SPK_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false, .bclk_inv = false, .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "TX std init fail"); return ret; }

    ret = i2s_channel_enable(tx_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "TX enable fail"); return ret; }
    tx_enabled = true;

    ESP_LOGI(TAG, "Speaker I2S TX ready");
    return ESP_OK;
}

/* ── 内部辅助函数 ───────────────────────────────────────────── */

/*
 * 确保 I2S 发送通道已使能。
 * 若未使能: 重新拉高 SD_MODE → 使能通道 → 写入 256 字节静音预热。
 * 用于流式播放时首次写入前的懒初始化。
 */
static esp_err_t ensure_tx_on(void)
{
    if (tx_enabled) return ESP_OK;
    gpio_set_level(CONFIG_VA_SPK_SD_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t ret = i2s_channel_enable(tx_chan);
    if (ret == ESP_OK) {
        tx_enabled = true;
        /* 写入一小段静音预热 DAC */
        static uint8_t silence[256];
        size_t n;
        i2s_channel_write(tx_chan, silence, sizeof(silence), &n, pdMS_TO_TICKS(10));
    }
    return ret;
}

/*
 * 将数据完整写入 I2S 通道（阻塞，循环直到全部写完）。
 *
 * @param chan I2S 通道句柄
 * @param data 数据缓冲区
 * @param len  字节长度
 * @return ESP_OK 全部写入成功
 */
static esp_err_t write_all(i2s_chan_handle_t chan, const uint8_t *data, size_t len)
{
    size_t total = 0;
    while (total < len) {
        size_t n = 0;
        esp_err_t r = i2s_channel_write(chan, data + total, len - total, &n, portMAX_DELAY);
        if (r != ESP_OK) return r;
        total += n;
    }
    return ESP_OK;
}

/* ── 扬声器公开 API ─────────────────────────────────────────── */

/*
 * 播放完整音频缓冲区（一次性）。
 * 流程: 确保 TX 使能 → 全量写入 → 停止 I2S + 关断功放 → 返回。
 * 适用场景: 播放提示音 / 问候语等短音频。
 */
esp_err_t voice_io_spk_play(const uint8_t *data, size_t len)
{
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ensure_tx_on();
    if (ret != ESP_OK) return ret;
    ret = write_all(tx_chan, data, len);
    voice_io_spk_stop();  /* 播放完毕立即静音 */
    return ret;
}

/*
 * 流式播放音频数据。
 * 不停止 I2S 通道，允许后续继续写入。需与 voice_io_spk_stop() 配对。
 * 适用场景: LLM 实时音频流，连续多次小量写入。
 */
esp_err_t voice_io_spk_play_stream(const uint8_t *data, size_t len)
{
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = ensure_tx_on();
    if (ret != ESP_OK) return ret;
    return write_all(tx_chan, data, len);
}

/*
 * 停止扬声器。
 * 流程: 写入 4096 字节静音清空 FIFO → 拉低 SD_MODE 关功放 → 禁用 I2S TX。
 * 可安全重复调用。
 */
esp_err_t voice_io_spk_stop(void)
{
    if (!tx_enabled) return ESP_OK;

    /* 发送静音帧清空 I2S FIFO，避免下一次播放时出现爆音 */
    uint8_t *silence = calloc(4096, 1);
    if (silence) {
        size_t n;
        i2s_channel_write(tx_chan, silence, 4096, &n, pdMS_TO_TICKS(100));
        free(silence);
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 关断功放 + 禁用通道 */
    gpio_set_level(CONFIG_VA_SPK_SD_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    i2s_channel_disable(tx_chan);
    tx_enabled = false;
    ESP_LOGI(TAG, "Speaker stopped");
    return ESP_OK;
}
