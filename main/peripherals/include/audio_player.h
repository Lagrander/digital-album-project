#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_EFFECT_CLICK = 0,
    AUDIO_EFFECT_STARTUP,
    AUDIO_EFFECT_AROMA_ON,
    AUDIO_EFFECT_AROMA_OFF,
    AUDIO_EFFECT_ERROR,
} audio_effect_t;

esp_err_t audio_player_init(void);

esp_err_t audio_player_play_effect(audio_effect_t effect);

esp_err_t audio_player_play_pcm(const int16_t *pcm_data,
                                size_t sample_count,
                                uint32_t sample_rate);

esp_err_t audio_player_play_wav_file(const char *path);

esp_err_t audio_player_play_tts_file(const char *path);

esp_err_t audio_player_stop(void);

bool audio_player_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif
