#include "i2s_mic_input.h"
#include "voice_io.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>

#include <string.h>

esp_err_t i2s_mic_input_init(void) {
    // 转发给底层的统一全双工驱动
    return voice_io_mic_init(I2S_MIC_INPUT_SAMPLE_RATE_HZ, 1, 16);
}

esp_err_t i2s_mic_input_read(int16_t *buffer, size_t sample_count,
                             size_t *samples_read, uint32_t timeout_ms) {
    if (buffer == NULL || samples_read == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_read = 0;

    esp_err_t ret = i2s_mic_input_init();
    if (ret != ESP_OK) return ret;

    // voice_io_mic_read 的 len 参数是需要读取的总字节数
    ret = voice_io_mic_read(false, buffer, sample_count * sizeof(int16_t));
    if (ret == ESP_OK) {
        // 由于 voice_io_mic_read 底层是 portMAX_DELAY 阻塞读满，因此默认全读到
        *samples_read = sample_count;
    }
    return ret;
}

esp_err_t i2s_mic_input_deinit(void) {
    // 全双工统一驱动不支持单独释放通道（会影响功放时钟），因此留空返回 OK
    return ESP_OK;
}

esp_err_t i2s_mic_input_read_rms(float *rms, uint32_t duration_ms) {
    if (rms == NULL || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t target_samples =
        ((size_t)I2S_MIC_INPUT_SAMPLE_RATE_HZ * duration_ms) / 1000;
    if (target_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t chunk_samples = 128;
    int16_t samples[128];
    size_t total_samples = 0;
    double square_sum = 0.0;

    while (total_samples < target_samples) {
        size_t samples_to_read = target_samples - total_samples;
        if (samples_to_read > chunk_samples) {
            samples_to_read = chunk_samples;
        }

        size_t samples_read = 0;
        esp_err_t ret =
            i2s_mic_input_read(samples, samples_to_read, &samples_read, 100);
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
    return ESP_OK;
}
