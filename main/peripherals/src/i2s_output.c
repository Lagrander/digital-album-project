#include "i2s_output.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "I2S_OUTPUT";

static i2s_chan_handle_t s_tx_chan;
static uint32_t s_sample_rate_hz = I2S_OUTPUT_DEFAULT_SAMPLE_RATE_HZ;

static const int16_t s_sine_table[64] = {
    0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31356, 32138, 32609,
    32767, 32609, 32138, 31356, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
    0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31356, -32138, -32609,
    -32767, -32609, -32138, -31356, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212,
};

static i2s_output_config_t i2s_output_default_config(void)
{
    return (i2s_output_config_t) {
        .bclk_gpio = I2S_OUTPUT_DEFAULT_BCLK_GPIO,
        .ws_gpio = I2S_OUTPUT_DEFAULT_WS_GPIO,
        .dout_gpio = I2S_OUTPUT_DEFAULT_DOUT_GPIO,
        .sample_rate_hz = I2S_OUTPUT_DEFAULT_SAMPLE_RATE_HZ,
    };
}

esp_err_t i2s_output_init(const i2s_output_config_t *config)
{
    if (s_tx_chan != NULL) {
        return ESP_OK;
    }

    i2s_output_config_t active_config = config ? *config : i2s_output_default_config();
    if (active_config.sample_rate_hz == 0) {
        active_config.sample_rate_hz = I2S_OUTPUT_DEFAULT_SAMPLE_RATE_HZ;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S TX channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(active_config.sample_rate_hz),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_MONO
        ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = active_config.bclk_gpio,
            .ws = active_config.ws_gpio,
            .dout = active_config.dout_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S STD mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    s_sample_rate_hz = active_config.sample_rate_hz;
    ESP_LOGI(TAG, "MAX98357A I2S output ready: BCLK=%d WS=%d DOUT=%d sample_rate=%lu",
             active_config.bclk_gpio,
             active_config.ws_gpio,
             active_config.dout_gpio,
             (unsigned long)s_sample_rate_hz);
    return ESP_OK;
}

esp_err_t i2s_output_deinit(void)
{
    if (s_tx_chan == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_tx_chan);
    esp_err_t del_ret = i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;

    return (ret != ESP_OK) ? ret : del_ret;
}

esp_err_t i2s_output_write(const int16_t *pcm_data, size_t sample_count)
{
    if (pcm_data == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tx_chan == NULL) {
        esp_err_t ret = i2s_output_init(NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const uint8_t *data = (const uint8_t *)pcm_data;
    size_t bytes_remaining = sample_count * sizeof(pcm_data[0]);

    while (bytes_remaining > 0) {
        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(
            s_tx_chan,
            data,
            bytes_remaining,
            &bytes_written,
            portMAX_DELAY
        );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            return ret;
        }
        if (bytes_written == 0) {
            return ESP_FAIL;
        }

        data += bytes_written;
        bytes_remaining -= bytes_written;
    }

    return ESP_OK;
}

esp_err_t i2s_output_play_sine(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (s_tx_chan == NULL) {
        esp_err_t ret = i2s_output_init(NULL);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (frequency_hz == 0 || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    enum {
        samples_per_chunk = 256,
        phase_fraction_bits = 16,
        amplitude_shift = 2,
    };

    int16_t samples[samples_per_chunk];
    const uint32_t total_samples = (s_sample_rate_hz * duration_ms) / 1000;
    const uint32_t table_phase_limit = (uint32_t)(64U << phase_fraction_bits);
    const uint32_t phase_step = (uint32_t)(((uint64_t)frequency_hz * table_phase_limit) / s_sample_rate_hz);
    uint32_t phase = 0;
    uint32_t samples_written = 0;

    while (samples_written < total_samples) {
        uint32_t chunk_samples = total_samples - samples_written;
        if (chunk_samples > samples_per_chunk) {
            chunk_samples = samples_per_chunk;
        }

        for (uint32_t i = 0; i < chunk_samples; ++i) {
            samples[i] = s_sine_table[phase >> phase_fraction_bits] >> amplitude_shift;
            phase += phase_step;
            while (phase >= table_phase_limit) {
                phase -= table_phase_limit;
            }
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_channel_write(
            s_tx_chan,
            samples,
            chunk_samples * sizeof(samples[0]),
            &bytes_written,
            portMAX_DELAY
        );
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
            return ret;
        }

        samples_written += bytes_written / sizeof(samples[0]);
    }

    memset(samples, 0, sizeof(samples));
    size_t bytes_written = 0;
    return i2s_channel_write(s_tx_chan, samples, sizeof(samples), &bytes_written, portMAX_DELAY);
}

esp_err_t i2s_output_play_test_tone(void)
{
    return i2s_output_play_sine(
        I2S_OUTPUT_DEFAULT_TONE_HZ,
        I2S_OUTPUT_DEFAULT_TONE_MS
    );
}
