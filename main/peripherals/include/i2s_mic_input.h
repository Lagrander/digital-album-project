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

/* ── 麦克风 I2S 引脚 ──────────────────────────────────────────
 * 引脚由 Kconfig (Voice Assistant 菜单) 注入
 */
#define MIC_I2S_BCLK_GPIO CONFIG_VA_MIC_SCK_PIN
#define MIC_I2S_WS_GPIO   CONFIG_VA_MIC_WS_PIN
#define MIC_I2S_DIN_GPIO  CONFIG_VA_MIC_SD_PIN

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
