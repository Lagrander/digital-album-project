#ifndef I2S_MIC_INPUT_H
#define I2S_MIC_INPUT_H

#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_MIC_INPUT_SAMPLE_RATE_HZ 16000
#define I2S_MIC_INPUT_BITS_PER_SAMPLE 16

#ifndef MIC_I2S_BCLK_GPIO
#define MIC_I2S_BCLK_GPIO GPIO_NUM_12
#endif

#ifndef MIC_I2S_WS_GPIO
#define MIC_I2S_WS_GPIO GPIO_NUM_11
#endif

#ifndef MIC_I2S_DIN_GPIO
#define MIC_I2S_DIN_GPIO GPIO_NUM_13
#endif

esp_err_t i2s_mic_input_init(void);
esp_err_t i2s_mic_input_read(int16_t *buffer,
                             size_t sample_count,
                             size_t *samples_read,
                             uint32_t timeout_ms);
esp_err_t i2s_mic_input_deinit(void);
esp_err_t i2s_mic_input_read_rms(float *rms, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif
