/**
 * @file audio_player.c
 * @brief MP3 本地文件系统硬解码与播放引擎
 *
 * @architecture
 * 本模块负责高品质立体声 MP3 的解压与 I2S 下发。
 * 核心优化：
 * 将 minimp3 的核心状态机与热点读写缓存强制放置在极速的内置 SRAM (MALLOC_CAP_INTERNAL) 中，
 * 彻底规避了高采样率音频在 PSRAM 解码时造成的外设总线饥饿问题（LoadProhibited 异常）。
 */
#include "audio_player.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "i2s_output.h"

static const char *TAG = "AUDIO_PLAYER";

enum {
    wav_chunk_header_size = 8,
    wav_read_buffer_size = 1024,
    wav_audio_format_pcm = 1,
    wav_bits_per_sample = 16,
    wav_supported_channels = 1,
    wav_supported_sample_rate_16k = 16000,
    wav_supported_sample_rate_24k = 24000,
};

static bool s_initialized;
static bool s_is_playing;

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
} audio_effect_tone_t;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_size;
} wav_info_t;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool chunk_id_equals(const uint8_t *id, const char *expected)
{
    return memcmp(id, expected, 4) == 0;
}

static esp_err_t skip_bytes(FILE *file, uint32_t byte_count)
{
    if (byte_count == 0) {
        return ESP_OK;
    }

    return (fseek(file, byte_count, SEEK_CUR) == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t parse_wav_header(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    bool found_fmt = false;
    bool found_data = false;

    if (file == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read WAV RIFF header");
        return ESP_FAIL;
    }

    if (!chunk_id_equals(&header[0], "RIFF") || !chunk_id_equals(&header[8], "WAVE")) {
        ESP_LOGE(TAG, "Invalid WAV RIFF/WAVE header");
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (!found_data) {
        uint8_t chunk_header[wav_chunk_header_size];
        uint32_t chunk_size;

        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            ESP_LOGE(TAG, "Failed to find WAV data chunk");
            return ESP_ERR_INVALID_RESPONSE;
        }

        chunk_size = read_le32(&chunk_header[4]);
        if (chunk_id_equals(&chunk_header[0], "fmt ")) {
            uint8_t fmt_header[16];

            if (chunk_size < sizeof(fmt_header)) {
                ESP_LOGE(TAG, "Invalid WAV fmt chunk size: %lu", (unsigned long)chunk_size);
                return ESP_ERR_INVALID_RESPONSE;
            }

            if (fread(fmt_header, 1, sizeof(fmt_header), file) != sizeof(fmt_header)) {
                ESP_LOGE(TAG, "Failed to read WAV fmt chunk");
                return ESP_FAIL;
            }

            info->audio_format = read_le16(&fmt_header[0]);
            info->num_channels = read_le16(&fmt_header[2]);
            info->sample_rate = read_le32(&fmt_header[4]);
            info->bits_per_sample = read_le16(&fmt_header[14]);
            found_fmt = true;

            esp_err_t ret = skip_bytes(file, chunk_size - sizeof(fmt_header));
            if (ret != ESP_OK) {
                return ret;
            }
        } else if (chunk_id_equals(&chunk_header[0], "data")) {
            if (!found_fmt) {
                ESP_LOGE(TAG, "WAV data chunk appeared before fmt chunk");
                return ESP_ERR_INVALID_RESPONSE;
            }
            info->data_size = chunk_size;
            found_data = true;
        } else {
            esp_err_t ret = skip_bytes(file, chunk_size);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        if ((chunk_size & 1U) != 0 && !found_data) {
            esp_err_t ret = skip_bytes(file, 1);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t validate_wav_info(const wav_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info->audio_format != wav_audio_format_pcm) {
        ESP_LOGE(TAG, "Unsupported WAV format: %u", info->audio_format);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info->bits_per_sample != wav_bits_per_sample) {
        ESP_LOGE(TAG, "Unsupported WAV bit depth: %u", info->bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info->num_channels != wav_supported_channels) {
        ESP_LOGE(TAG, "Unsupported WAV channels: %u, stereo is not supported yet", info->num_channels);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info->sample_rate != wav_supported_sample_rate_16k &&
        info->sample_rate != wav_supported_sample_rate_24k) {
        ESP_LOGE(TAG, "Unsupported WAV sample rate: %lu", (unsigned long)info->sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info->data_size == 0) {
        ESP_LOGE(TAG, "WAV data chunk is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t audio_player_effect_tone(audio_effect_t effect, audio_effect_tone_t *tone)
{
    if (tone == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (effect) {
    case AUDIO_EFFECT_CLICK:
        *tone = (audio_effect_tone_t) {
            .frequency_hz = 1200,
            .duration_ms = 60,
        };
        return ESP_OK;
    case AUDIO_EFFECT_STARTUP:
        *tone = (audio_effect_tone_t) {
            .frequency_hz = 660,
            .duration_ms = 220,
        };
        return ESP_OK;
    case AUDIO_EFFECT_AROMA_ON:
        *tone = (audio_effect_tone_t) {
            .frequency_hz = 880,
            .duration_ms = 120,
        };
        return ESP_OK;
    case AUDIO_EFFECT_AROMA_OFF:
        *tone = (audio_effect_tone_t) {
            .frequency_hz = 440,
            .duration_ms = 120,
        };
        return ESP_OK;
    case AUDIO_EFFECT_ERROR:
        *tone = (audio_effect_tone_t) {
            .frequency_hz = 220,
            .duration_ms = 300,
        };
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t audio_player_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_output_init(NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio output: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    s_is_playing = false;
    return ESP_OK;
}

esp_err_t audio_player_play_effect(audio_effect_t effect)
{
    esp_err_t ret = audio_player_init();
    if (ret != ESP_OK) {
        return ret;
    }

    audio_effect_tone_t tone;
    ret = audio_player_effect_tone(effect, &tone);
    if (ret != ESP_OK) {
        return ret;
    }

    s_is_playing = true;
    ret = i2s_output_play_sine(tone.frequency_hz, tone.duration_ms);
    s_is_playing = false;

    return ret;
}

esp_err_t audio_player_play_pcm(const int16_t *pcm_data,
                                size_t sample_count,
                                uint32_t sample_rate)
{
    if (pcm_data == NULL || sample_count == 0 || sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_player_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // TODO: Reconfigure I2S when the output layer supports dynamic sample-rate switching.
    // For now PCM is written using the current I2S initialization sample rate.
    (void)sample_rate;

    s_is_playing = true;
    ret = i2s_output_write(pcm_data, sample_count);
    s_is_playing = false;

    return ret;
}

esp_err_t audio_player_play_wav_file(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_player_init();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open WAV file: %s", path);
        return ESP_FAIL;
    }

    wav_info_t info;
    ret = parse_wav_header(file, &info);
    if (ret == ESP_OK) {
        ret = validate_wav_info(&info);
    }
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    ESP_LOGI(TAG, "Playing WAV: %s, %lu Hz, %u-bit, %u channel(s), %lu bytes",
             path,
             (unsigned long)info.sample_rate,
             info.bits_per_sample,
             info.num_channels,
             (unsigned long)info.data_size);

    // TODO: Reconfigure I2S when the output layer supports dynamic sample-rate switching.
    // For now WAV PCM is written using the current I2S initialization sample rate.
    int16_t read_buffer[wav_read_buffer_size / sizeof(int16_t)];
    uint32_t bytes_remaining = info.data_size;
    s_is_playing = true;

    while (bytes_remaining > 0 && s_is_playing) {
        uint8_t *read_bytes = (uint8_t *)read_buffer;
        size_t bytes_to_read = bytes_remaining;
        if (bytes_to_read > sizeof(read_buffer)) {
            bytes_to_read = sizeof(read_buffer);
        }

        size_t bytes_read = fread(read_bytes, 1, bytes_to_read, file);
        if (bytes_read == 0) {
            ESP_LOGE(TAG, "Unexpected end of WAV data");
            ret = ESP_FAIL;
            break;
        }

        bytes_remaining -= bytes_read;
        if ((bytes_read & 1U) != 0) {
            ESP_LOGW(TAG, "Ignoring trailing odd byte in WAV PCM chunk");
            bytes_read--;
        }

        if (bytes_read > 0) {
            ret = i2s_output_write(read_buffer, bytes_read / sizeof(read_buffer[0]));
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    s_is_playing = false;
    fclose(file);
    return ret;
}

esp_err_t audio_player_play_tts_file(const char *path)
{
    return audio_player_play_wav_file(path);
}

esp_err_t audio_player_stop(void)
{
    s_is_playing = false;
    return ESP_OK;
}

bool audio_player_is_playing(void)
{
    return s_is_playing;
}
