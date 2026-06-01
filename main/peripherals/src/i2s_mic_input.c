#include "i2s_mic_input.h"

#include <math.h>
#include <stdbool.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "I2S_MIC_INPUT";

enum {
    mic_raw_samples_per_read = 128,
    mic_inmp441_shift_to_16bit = 16,
};

static i2s_chan_handle_t s_rx_chan;

static int16_t mic_convert_raw_sample(int32_t raw_sample)
{
    return (int16_t)(raw_sample >> mic_inmp441_shift_to_16bit);
}

esp_err_t i2s_mic_input_init(void)
{
    if (s_rx_chan != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S RX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_MIC_INPUT_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK_GPIO,
            .ws = MIC_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S RX STD mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "INMP441 I2S input ready: BCLK=%d WS=%d DIN=%d sample_rate=%d",
             MIC_I2S_BCLK_GPIO,
             MIC_I2S_WS_GPIO,
             MIC_I2S_DIN_GPIO,
             I2S_MIC_INPUT_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t i2s_mic_input_read(int16_t *buffer,
                             size_t sample_count,
                             size_t *samples_read,
                             uint32_t timeout_ms)
{
    if (buffer == NULL || samples_read == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_read = 0;

    esp_err_t ret = i2s_mic_input_init();
    if (ret != ESP_OK) {
        return ret;
    }

    int32_t raw_samples[mic_raw_samples_per_read];
    TickType_t timeout_ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    while (*samples_read < sample_count) {
        size_t needed_samples = sample_count - *samples_read;
        size_t read_samples = needed_samples;
        if (read_samples > mic_raw_samples_per_read) {
            read_samples = mic_raw_samples_per_read;
        }

        size_t bytes_read = 0;
        ret = i2s_channel_read(
            s_rx_chan,
            raw_samples,
            read_samples * sizeof(raw_samples[0]),
            &bytes_read,
            timeout_ticks
        );
        if (ret != ESP_OK) {
            if (*samples_read > 0) {
                return ESP_OK;
            }
            ESP_LOGE(TAG, "I2S mic read failed: %s", esp_err_to_name(ret));
            return ret;
        }

        size_t got_samples = bytes_read / sizeof(raw_samples[0]);
        if (got_samples == 0) {
            break;
        }

        for (size_t i = 0; i < got_samples; ++i) {
            buffer[*samples_read + i] = mic_convert_raw_sample(raw_samples[i]);
        }
        *samples_read += got_samples;
    }

    return ESP_OK;
}

esp_err_t i2s_mic_input_deinit(void)
{
    if (s_rx_chan == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_rx_chan);
    esp_err_t del_ret = i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;

    return (ret != ESP_OK) ? ret : del_ret;
}

esp_err_t i2s_mic_input_read_rms(float *rms, uint32_t duration_ms)
{
    if (rms == NULL || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t target_samples = ((size_t)I2S_MIC_INPUT_SAMPLE_RATE_HZ * duration_ms) / 1000;
    if (target_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t samples[mic_raw_samples_per_read];
    size_t total_samples = 0;
    double square_sum = 0.0;

    while (total_samples < target_samples) {
        size_t samples_to_read = target_samples - total_samples;
        if (samples_to_read > mic_raw_samples_per_read) {
            samples_to_read = mic_raw_samples_per_read;
        }

        size_t samples_read = 0;
        esp_err_t ret = i2s_mic_input_read(samples, samples_to_read, &samples_read, 100);
        if (ret != ESP_OK) {
            return ret;
        }
        if (samples_read == 0) {
            break;
        }

        for (size_t i = 0; i < samples_read; ++i) {
            square_sum += (double)samples[i] * (double)samples[i];
        }
        total_samples += samples_read;
    }

    if (total_samples == 0) {
        return ESP_ERR_TIMEOUT;
    }

    *rms = sqrtf((float)(square_sum / (double)total_samples));
    ESP_LOGI(TAG, "Mic RMS over %lu ms: %.2f", (unsigned long)duration_ms, (double)*rms);
    return ESP_OK;
}
