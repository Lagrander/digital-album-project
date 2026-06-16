#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include "driver/gpio.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_OUTPUT_DEFAULT_SAMPLE_RATE_HZ 16000
#define I2S_OUTPUT_DEFAULT_BITS_PER_SAMPLE 16
#define I2S_OUTPUT_DEFAULT_TONE_HZ 440
#define I2S_OUTPUT_DEFAULT_TONE_MS 1000

/* ── 扬声器 I2S 引脚 ──────────────────────────────────────────
 * 引脚由 Kconfig (Voice Assistant 菜单) 注入
 */
#define I2S_OUTPUT_DEFAULT_BCLK_GPIO CONFIG_VA_SPK_BCLK_PIN
#define I2S_OUTPUT_DEFAULT_WS_GPIO CONFIG_VA_SPK_LRCK_PIN
#define I2S_OUTPUT_DEFAULT_DOUT_GPIO CONFIG_VA_SPK_DIN_PIN

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
