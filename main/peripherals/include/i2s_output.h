#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_OUTPUT_DEFAULT_SAMPLE_RATE_HZ  16000
#define I2S_OUTPUT_DEFAULT_BITS_PER_SAMPLE 16
#define I2S_OUTPUT_DEFAULT_TONE_HZ         440
#define I2S_OUTPUT_DEFAULT_TONE_MS         1000

#ifndef I2S_OUTPUT_DEFAULT_BCLK_GPIO
#define I2S_OUTPUT_DEFAULT_BCLK_GPIO GPIO_NUM_4
#endif

#ifndef I2S_OUTPUT_DEFAULT_WS_GPIO
#define I2S_OUTPUT_DEFAULT_WS_GPIO GPIO_NUM_5
#endif

#ifndef I2S_OUTPUT_DEFAULT_DOUT_GPIO
#define I2S_OUTPUT_DEFAULT_DOUT_GPIO GPIO_NUM_6
#endif

typedef struct {
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t dout_gpio;
    uint32_t sample_rate_hz;
} i2s_output_config_t;

esp_err_t i2s_output_init(const i2s_output_config_t *config);
esp_err_t i2s_output_deinit(void);
esp_err_t i2s_output_write(const int16_t *pcm_data, size_t sample_count);
esp_err_t i2s_output_play_sine(uint32_t frequency_hz, uint32_t duration_ms);
esp_err_t i2s_output_play_test_tone(void);

#ifdef __cplusplus
}
#endif

#endif
