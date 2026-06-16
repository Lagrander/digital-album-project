/*

 * voice_io.c — I2S 音频驱动实现（48kHz 全双工 + PSRAM + FIR降采样重构版）

 */

#include "voice_io.h"

#include "driver/gpio.h"

#include "driver/i2s_std.h"

#include "dsps_fir.h"

#include "esp_check.h"

#include "esp_dsp.h"

#include "esp_heap_caps.h"

#include "esp_log.h"

#include "freertos/FreeRTOS.h"

#include "freertos/idf_additions.h"

#include "freertos/ringbuf.h"

#include "freertos/task.h"

#include <math.h>

#include <string.h>

#ifndef M_PI

#define M_PI 3.14159265358979323846

#endif

static const char *TAG = "voice_io";

/* ── 内部状态与结构 ────────────────────────── */

static i2s_chan_handle_t rx_chan = NULL;

static i2s_chan_handle_t tx_chan = NULL;

static bool s_i2s_shared_initialized = false;

static bool s_amp_enabled = false;

// PSRAM 缓冲池

static RingbufHandle_t tx_ringbuf = NULL;

static RingbufHandle_t rx_ringbuf = NULL;

static uint8_t s_spk_volume =
    10; // 全局硬件音量 (0~100)，默认 10% 防止瞬间大电流触发功放断电保护

void voice_io_set_spk_volume(uint8_t vol) {

  if (vol > 100)
    vol = 100;

  s_spk_volume = vol;

  ESP_LOGI(TAG, "Speaker volume set to %d%%", vol);
}

uint8_t voice_io_get_spk_volume(void) { return s_spk_volume; }

// FIR 滤波器所需资源

// ESP-S3 DSP库的汇编指令要求滤波器阶数必须 be 4 的倍数，所以设为 32

// 并且系数和延迟线数组必须按 16 字节对齐

#define FIR_TAP_NUM 32

static __attribute__((aligned(16))) float fir_coeffs[FIR_TAP_NUM];

static __attribute__((aligned(16))) float fir_delayline[FIR_TAP_NUM];

static fir_f32_t fir_decim_obj;

/* ── FIR 滤波器初始化 (48kHz -> 16kHz) ──────── */

static void init_fir_decim(void) {

  float wc = M_PI / 3.0f; // 48kHz 到 16kHz 抽取，截止频率 8kHz (即 1/3 Nyquist)

  float sum = 0;

  float center = (FIR_TAP_NUM - 1) / 2.0f;

  for (int i = 0; i < FIR_TAP_NUM; i++) {

    float x = i - center;

    if (x == 0.0f) {

      fir_coeffs[i] = wc / M_PI;

    } else {

      fir_coeffs[i] = sinf(wc * x) / (M_PI * x);
    }

    // 应用 Hanning 窗抗振铃

    fir_coeffs[i] *= 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FIR_TAP_NUM - 1)));

    sum += fir_coeffs[i];
  }

  // 归一化

  for (int i = 0; i < FIR_TAP_NUM; i++) {

    fir_coeffs[i] /= sum;
  }

  // 初始化 esp-dsp 汇编级抽取器，抽取比例 3

  esp_err_t ret = dsps_fird_init_f32(&fir_decim_obj, fir_coeffs, fir_delayline,

                                     FIR_TAP_NUM, 3);

  if (ret == ESP_OK) {

    ESP_LOGI(TAG, "FIR decimation filter initialized.");

  } else {

    ESP_LOGE(TAG, "FIR filter init failed: %s", esp_err_to_name(ret));
  }
}

/* ── Audio Tasks ────────────────────────── */

#define AUDIO_CHUNK_SAMPLES 480 // 48kHz 下的 10ms

static void audio_rx_task(void *args) {

  int32_t *raw_48k_buf =

      heap_caps_aligned_alloc(16, AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t),

                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  float *pcm_48k_f =

      heap_caps_aligned_alloc(16, AUDIO_CHUNK_SAMPLES * sizeof(float),

                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  float *pcm_16k_f =

      heap_caps_aligned_alloc(16, (AUDIO_CHUNK_SAMPLES / 3) * sizeof(float),

                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  int16_t *pcm_16k_buf =

      heap_caps_aligned_alloc(16, (AUDIO_CHUNK_SAMPLES / 3) * sizeof(int16_t),

                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!raw_48k_buf || !pcm_48k_f || !pcm_16k_f || !pcm_16k_buf) {

    ESP_LOGE(TAG, "Failed to allocate aligned buffers for RX task");

    vTaskDelete(NULL);

    return;
  }

  while (1) {

    size_t bytes_read = 0;

    // 1. 从内部 SRAM 的 DMA 极速读取 48kHz 硬件流 (双通道大小)

    esp_err_t ret = i2s_channel_read(rx_chan, raw_48k_buf,

                                     AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t),

                                     &bytes_read, pdMS_TO_TICKS(50));

    if (ret == ESP_OK &&
        bytes_read == AUDIO_CHUNK_SAMPLES * 2 * sizeof(int32_t)) {

      static int debug_cnt = 0;
      long long l_sum = 0, r_sum = 0;

      // 2. 将 32-bit (高 24 位有效) 转为 16-bit
      // 浮点，并施加数字增益(x4.0)以提高唤醒灵敏度 融合左右声道：防止有些劣质
      // INMP441 模块将 L/R 默认拉高输出到右声道
      for (int i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
        int16_t left_val = (int16_t)(raw_48k_buf[i * 2] >> 16);
        int16_t right_val = (int16_t)(raw_48k_buf[i * 2 + 1] >> 16);

        l_sum += abs(left_val);
        r_sum += abs(right_val);

        // 如果左声道是 0，强行用右声道（兼容接反的情况），如果都有声音就混音
        int16_t mix_val =
            (left_val == 0 && right_val != 0) ? right_val : left_val;

        // 智能噪声门 (Noise Gate) 与适度增益：
        // 底噪极其微弱（±15以内）时，直接归零，彻底消灭环境白噪声和电流声！
        // 这将大幅提高 ASR 的识别准确率，并防止 VAD 误判为持续有人说话。
        if (mix_val > -100 && mix_val < 100) {
            pcm_48k_f[i] = 0.0f;
        } else {
            // 对有效语音保持 24.0 倍放大，确保 WakeNet 能稳定唤醒，但不过度失真
            pcm_48k_f[i] = (float)mix_val * 24.0f;
        }
      }

      // if (++debug_cnt % 100 == 0) {
      //   ESP_LOGI(TAG, "🎙️ Mic Energy -> L: %lld, R: %lld", l_sum /
      //   AUDIO_CHUNK_SAMPLES, r_sum / AUDIO_CHUNK_SAMPLES);
      // }

      // 3. FIR 抗混叠低通滤波 + 3:1 抽取 (48k -> 16k)

      dsps_fird_f32(&fir_decim_obj, pcm_48k_f, pcm_16k_f,

                    AUDIO_CHUNK_SAMPLES / 3);

      // 4. 浮点转回 16-bit PCM（tanh 软限制，避免硬切爆破音）

      for (int i = 0; i < AUDIO_CHUNK_SAMPLES / 3; i++) {

        float val = pcm_16k_f[i];
        // tanh 软限制三步曲：
        //   1) 归一化：val / 32768  →  将 ±32768 映射到 ±1.0
        //   2) tanh 压缩：tanh(norm)  →  小信号近似线性通过，
        //                                 大信号渐进平滑到 ±1.0，无陡峭阶跃
        //   3) 还原：×32767  →  回到 int16 满量程范围
        // 对比硬切：原来 >32767 直接截断，产生高频咔哒声；
        //           现在 tanh 平滑过渡，波形连续，无爆破音
        float norm = val / 32768.0f;
        if (norm > 3.0f)
          norm = 3.0f; // tanh(3) ≈ 0.995，已接近饱和
        if (norm < -3.0f)
          norm = -3.0f;
        float limited = tanhf(norm) * 32767.0f;
        pcm_16k_buf[i] = (int16_t)limited;
      }

      // 5. 送入 PSRAM 缓冲池，让上层网络慢慢消费

      xRingbufferSend(rx_ringbuf, pcm_16k_buf,

                      (AUDIO_CHUNK_SAMPLES / 3) * sizeof(int16_t), 0);
    }
  }
}

// 用于抵御 Ringbuffer 奇数截断错位的终极缝合缓存

static uint8_t s_leftover_byte = 0;

static bool s_has_leftover = false;

static void audio_tx_task(void *args) {

  // I2S DMA 从 PSRAM 取数据经测试无卡死问题，将 upmix_buf 移到 PSRAM 减轻内置
  // SRAM 压力

  // 每次最多接收 512 字节(256 个 16-bit 采样)。
  // 上采样 3 倍后，变成 768 个 16-bit 采样。
  // 转为双声道 32-bit 后，总字节数为 768 * 8 = 6144 字节。
  int32_t *upmix_buf =
      heap_caps_malloc(6144, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  if (!upmix_buf) {

    ESP_LOGE(TAG, "Failed to allocate upmix_buf in PSRAM for TX task");

    vTaskDelete(NULL);

    return;
  }

  // tx_ringbuf 里存的是 16kHz 16-bit Mono 数据

  uint32_t total_bytes_rx = 0;

  uint32_t total_samples_tx = 0;

  uint32_t i2s_err_count = 0;

  TickType_t last_log = xTaskGetTickCount();

  while (1) {

    size_t size;

    // 等待上层写入数据，每次最多接收 512 字节

    uint8_t *raw_data =

        (uint8_t *)xRingbufferReceiveUpTo(tx_ringbuf, &size, pdMS_TO_TICKS(10),
                                          512);

    if (raw_data) {

      total_bytes_rx += size;

      size_t byte_idx = 0;

      size_t sample_cnt = 0;

      // 护盾第 1 层：如果上次落单了半个字节，且这次有新数据，立刻缝合！

      if (s_has_leftover && size > 0) {

        int16_t assembled = s_leftover_byte | ((uint16_t)raw_data[0] << 8);

        byte_idx = 1;

        s_has_leftover = false;

        // 硬件级全局音量控制

        assembled = (int16_t)((int32_t)assembled * s_spk_volume / 100);

        int32_t val = ((int32_t)assembled) << 16;

        upmix_buf[sample_cnt * 6 + 0] = val;

        upmix_buf[sample_cnt * 6 + 1] = val;

        upmix_buf[sample_cnt * 6 + 2] = val;

        upmix_buf[sample_cnt * 6 + 3] = val;

        upmix_buf[sample_cnt * 6 + 4] = val;

        upmix_buf[sample_cnt * 6 + 5] = val;

        sample_cnt++;
      }

      // 护盾第 2 层：计算剩下的数据中成对的字节进行处理

      while (byte_idx + 1 < size) {

        int16_t val_16 =
            raw_data[byte_idx] | ((uint16_t)raw_data[byte_idx + 1] << 8);

        byte_idx += 2;

        // 硬件级全局音量控制

        val_16 = (int16_t)((int32_t)val_16 * s_spk_volume / 100);

        int32_t val = ((int32_t)val_16) << 16;

        upmix_buf[sample_cnt * 6 + 0] = val;

        upmix_buf[sample_cnt * 6 + 1] = val;

        upmix_buf[sample_cnt * 6 + 2] = val;

        upmix_buf[sample_cnt * 6 + 3] = val;

        upmix_buf[sample_cnt * 6 + 4] = val;

        upmix_buf[sample_cnt * 6 + 5] = val;

        sample_cnt++;
      }

      // 护盾第 3 层：如果末尾多出一个"落单"的奇数字节，把它攥在手里

      if (byte_idx < size) {

        s_leftover_byte = raw_data[byte_idx];

        s_has_leftover = true;
      }

      // 送入 I2S DMA

      if (sample_cnt > 0) {

        size_t bytes_written = 0;

        esp_err_t err = i2s_channel_write(tx_chan, upmix_buf, sample_cnt * 24,

                                          &bytes_written, pdMS_TO_TICKS(100));

        if (err != ESP_OK) {

          i2s_err_count++;

          if (i2s_err_count <= 3 || i2s_err_count % 100 == 0) {

            ESP_LOGW(TAG, "I2S DMA Write Failed (%lu): %s",
                     (unsigned long)i2s_err_count, esp_err_to_name(err));
          }
        }

        total_samples_tx += sample_cnt;
      }

      vRingbufferReturnItem(tx_ringbuf, (void *)raw_data);
    }

    // 每 30 秒打印一次消费速率

    TickType_t now = xTaskGetTickCount();

    if (now - last_log >= pdMS_TO_TICKS(30000)) {

      ESP_LOGI(TAG,
               "TX stats: %lu KB received, %lu samples sent, %lu I2S errors",

               (unsigned long)(total_bytes_rx / 1024),

               (unsigned long)total_samples_tx,

               (unsigned long)i2s_err_count);

      last_log = now;
    }
  }
}

/* ── 统一全双工初始化逻辑 ────────────────────────────── */

static esp_err_t voice_io_shared_init(uint32_t unused_rate) {

  if (s_i2s_shared_initialized) {

    return ESP_OK;
  }

  esp_err_t ret;

  uint32_t hw_sample_rate = 48000; // 强制底层硬件 48kHz

  /* 0. 初始化缓冲池与 DSP (PSRAM) */

  size_t rb_size = 64 * 1024; // 64KB 巨大缓冲池

  uint8_t *tx_buf = heap_caps_malloc(rb_size, MALLOC_CAP_SPIRAM);

  StaticRingbuffer_t *tx_struct = heap_caps_malloc(

      sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  tx_ringbuf =

      xRingbufferCreateStatic(rb_size, RINGBUF_TYPE_BYTEBUF, tx_buf, tx_struct);

  uint8_t *rx_buf = heap_caps_malloc(rb_size, MALLOC_CAP_SPIRAM);

  StaticRingbuffer_t *rx_struct = heap_caps_malloc(

      sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  rx_ringbuf =

      xRingbufferCreateStatic(rb_size, RINGBUF_TYPE_BYTEBUF, rx_buf, rx_struct);

  init_fir_decim();

  /* 1. 初始化 MAX98357A SD_MODE 功放使能管脚（ESP32 原生 GPIO 直驱）

   *    PCF8574 弱上拉无法可靠拉高 SD_MODE（实测 P3 仅 0.15V），改用 GPIO */

#if VA_SPK_SD_PIN >= 0

  gpio_config_t sd_cfg = {

      .pin_bit_mask = BIT64(VA_SPK_SD_PIN),

      .mode = GPIO_MODE_OUTPUT,

      .pull_up_en = GPIO_PULLUP_DISABLE,

      .pull_down_en = GPIO_PULLDOWN_DISABLE,

      .intr_type = GPIO_INTR_DISABLE,

  };

  gpio_config(&sd_cfg);

  gpio_set_level(VA_SPK_SD_PIN, 0);

  s_amp_enabled = false;

  vTaskDelay(pdMS_TO_TICKS(50));

  ESP_LOGI(TAG, "AMP SD_MODE on GPIO %d, initial LOW", VA_SPK_SD_PIN);

#endif

  /* 2. 同时申请 I2S_NUM_0 的 RX 和 TX 全双工通道 */

  i2s_chan_config_t chan_cfg =

      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

  chan_cfg.auto_clear = true;

  // 优化 I2S DMA 缓冲区大小以解决加载 WakeNet 模型后的 SRAM 内存不足问题

  chan_cfg.dma_desc_num = 4;

  chan_cfg.dma_frame_num = 240;

  ret = i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan);

  if (ret != ESP_OK)

    return ret;

  /* 3. 基础配置：32位槽宽，共享 BCLK/WS */

  i2s_std_config_t std_cfg = {

      .clk_cfg =

          {

              .sample_rate_hz = hw_sample_rate,

              .clk_src = I2S_CLK_SRC_DEFAULT,

              .mclk_multiple = I2S_MCLK_MULTIPLE_256,

          },

      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,

                                                      I2S_SLOT_MODE_STEREO),

      .gpio_cfg =

          {

              .mclk = I2S_GPIO_UNUSED,

              .bclk = VA_MIC_SCK_PIN,

              .ws = VA_MIC_WS_PIN,

              .dout = VA_SPK_DIN_PIN,

              .din = VA_MIC_SD_PIN,

              .invert_flags = {false, false, false},

          },

  };

  /* 4. 配置 TX (扬声器) */
  i2s_std_config_t tx_std_cfg = std_cfg;
  tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  tx_std_cfg.gpio_cfg.din =
      I2S_GPIO_UNUSED; // 扬声器不需要数据输入引脚，避免冲突
  // 全双工模式下 BCLK/WS 由 RX 主通道驱动，TX 保留相同引脚配置以确保 GPIO
  // 矩阵正确路由时钟
  ret = i2s_channel_init_std_mode(tx_chan, &tx_std_cfg);

  if (ret != ESP_OK)

    return ret;

  /* 5. 配置 RX (麦克风) */

  i2s_std_config_t rx_std_cfg = std_cfg;

  rx_std_cfg.slot_cfg.slot_mode =
      I2S_SLOT_MODE_STEREO; // 配置为立体声以恢复64BCLK，物理上与TX对齐

  rx_std_cfg.slot_cfg.slot_mask =
      I2S_STD_SLOT_BOTH; // 启用双声道物理掩码，让 ESP-IDF 计算标准的 64 BCLK

  // 共享时钟全双工配置下，RX 麦克风作为主通道，占用物理引脚提供持续时钟

  rx_std_cfg.gpio_cfg.bclk = VA_MIC_SCK_PIN;

  rx_std_cfg.gpio_cfg.ws = VA_MIC_WS_PIN;

  rx_std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;

  ret = i2s_channel_init_std_mode(rx_chan, &rx_std_cfg);

  if (ret != ESP_OK)

    return ret;

  /* 6. 使能通道 (先使能 Slave TX，后使能 Master RX，保障时钟生成时接收方已就绪)
   */

  i2s_channel_enable(tx_chan);

  i2s_channel_enable(rx_chan);

  /* ── 硬件诊断：直接读 SD 引脚物理电平 ──
   * gpio_get_level() 不依赖引脚功能，直接读物理电平。
   * - 如果一直是 0: INMP441 的 SD 被持续拉低（没通电 / 没时钟 / 芯片坏）
   * - 如果一直是 1: INMP441 的 SD 被持续拉高（芯片没在输出）
   * - 如果有 0 有 1: 芯片在输出，但 I2S 同步/格式有问题
   */
  {
    int zeros = 0, ones = 0;
    for (int i = 0; i < 200; i++) {
      if (gpio_get_level(VA_MIC_SD_PIN))
        ones++;
      else
        zeros++;
      esp_rom_delay_us(10);
    }
    ESP_LOGI(TAG, "🔍 HW diag: SD(GPIO%d) 200 samples → low=%d, high=%d",
             VA_MIC_SD_PIN, zeros, ones);
  }

  /* 7. 创建高优先级搬运任务 (Core 1 避免干扰 Core 0 的 Wi-Fi)

   *    使用 PSRAM 栈避免碎片化的内置 SRAM 无法分配大块连续内存 */

  BaseType_t rx_ret = xTaskCreatePinnedToCoreWithCaps(

      audio_rx_task, "audio_rx", 6144, NULL, configMAX_PRIORITIES - 2, NULL, 1,

      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (rx_ret != pdPASS) {

    ESP_LOGE(TAG,
             "FATAL: audio_rx_task creation failed (err %d) — no mic input!",
             rx_ret);
  }

  BaseType_t tx_ret = xTaskCreatePinnedToCoreWithCaps(

      audio_tx_task, "audio_tx", 8192, NULL, configMAX_PRIORITIES - 2, NULL, 1,

      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (tx_ret != pdPASS) {

    ESP_LOGE(
        TAG,
        "FATAL: audio_tx_task creation failed (err %d) — no speaker output!",
        tx_ret);
  }

  s_i2s_shared_initialized = true;

  ESP_LOGI(TAG, "Professional Duplex I2S initialized (48kHz HW -> 16kHz SW).");

  return ESP_OK;
}

/* ── 麦克风 API (抽取缓冲池数据) ──────────────────── */

esp_err_t voice_io_mic_init(uint32_t sample_rate, int channel,

                            int bits_per_sample) {

  return voice_io_shared_init(sample_rate);
}

esp_err_t voice_io_mic_read(bool is_raw, int16_t *buffer, int len) {

  if (!s_i2s_shared_initialized)

    return ESP_ERR_INVALID_STATE;

  size_t received = 0;

  while (received < (size_t)len) {

    size_t size;

    void *data = xRingbufferReceiveUpTo(rx_ringbuf, &size, portMAX_DELAY,

                                        len - received);

    if (data) {

      memcpy((uint8_t *)buffer + received, data, size);

      vRingbufferReturnItem(rx_ringbuf, data);

      received += size;
    }
  }

  return ESP_OK;
}

int voice_io_mic_channel(void) { return 1; }

void voice_io_mic_clear(void) {

  if (!s_i2s_shared_initialized || !rx_ringbuf)

    return;

  size_t size;

  void *item;

  while ((item = xRingbufferReceive(rx_ringbuf, &size, 0)) != NULL) {

    vRingbufferReturnItem(rx_ringbuf, item);
  }

  ESP_LOGI(TAG, "Mic RX ringbuf cleared");
}

/* ── 扬声器 API (推入缓冲池) ───────────────────────── */

esp_err_t voice_io_spk_init(uint32_t sample_rate, int channel,

                            int bits_per_sample) {

  return voice_io_shared_init(sample_rate);
}

static esp_err_t ensure_tx_on(void) {

  if (!s_i2s_shared_initialized)

    return ESP_ERR_INVALID_STATE;

  if (s_amp_enabled)

    return ESP_OK; // 缓存命中

  /* 【关键】功放启动前清空 RingBuffer 脏数据，防止播放第一帧爆出噪音 */

  if (tx_ringbuf) {
    size_t size;
    void *item;
    while ((item = xRingbufferReceive(tx_ringbuf, &size, 0)) != NULL)
      vRingbufferReturnItem(tx_ringbuf, item);
  }

#if VA_SPK_SD_PIN >= 0

  // 冷启动复位序列（原生 GPIO 推挽输出，驱动力充足）：

  gpio_set_level(VA_SPK_SD_PIN, 0);

  vTaskDelay(pdMS_TO_TICKS(100));

  gpio_set_level(VA_SPK_SD_PIN, 1);

  vTaskDelay(pdMS_TO_TICKS(50)); // 延长稳定时间，确保功放 PLL 锁相就绪

#else

  /* SD_MODE 接 3.3V 常通时，等一下让 I2S 通道有时间输出静音帧 */

  vTaskDelay(pdMS_TO_TICKS(30));

#endif

  /* 【关键】填充 30ms 静音帧，平滑启动：功放通电后先输出 0 值，避免 DC
   * 阶跃爆破声 */

  if (tx_ringbuf) {
    int16_t silence[240]; /* 16kHz 30ms = 480 采样 */
    for (int i = 0; i < 240; i++)
      silence[i] = 0;
    xRingbufferSend(tx_ringbuf, silence, 240 * 2, pdMS_TO_TICKS(50));
  }

  s_amp_enabled = true;

#if VA_SPK_SD_PIN >= 0

  ESP_LOGI(TAG, "AMP enabled (GPIO %d high)", VA_SPK_SD_PIN);

#else

  ESP_LOGI(TAG, "AMP always-on (SD_MODE floating, no GPIO)");

#endif

  return ESP_OK;
}

void voice_io_spk_force_reset(void) {

  if (!s_i2s_shared_initialized)

    return;

  s_amp_enabled = false; // 强制清除缓存状态

#if VA_SPK_SD_PIN >= 0

  gpio_set_level(VA_SPK_SD_PIN, 0);

  vTaskDelay(pdMS_TO_TICKS(100)); // 强制物理拉低 100ms 模拟物理断电

  gpio_set_level(VA_SPK_SD_PIN, 1);

  vTaskDelay(pdMS_TO_TICKS(50)); // 延时 50ms 等待 PLL 重新稳定锁相

  s_amp_enabled = true;

  ESP_LOGI(TAG, "Speaker forced hardware reset (anti-shake & PLL lock)");

#else

  ESP_LOGI(TAG, "Speaker always-on (no GPIO control), skip forced reset");

#endif
}

esp_err_t voice_io_spk_play(const uint8_t *data, size_t len) {

  return voice_io_spk_play_stream(data, len);
}

esp_err_t voice_io_spk_play_stream(const uint8_t *data, size_t len) {

  if (!data || !len)

    return ESP_ERR_INVALID_ARG;

  esp_err_t ret = ensure_tx_on();

  if (ret != ESP_OK)

    return ret;

  // 分块发送，防止超过 RingBuffer 一次性发送限制

  size_t sent = 0;

  while (sent < len) {

    size_t to_send = len - sent;

    if (to_send > 4096)

      to_send = 4096;

    if (xRingbufferSend(tx_ringbuf, (void *)(data + sent), to_send,

                        pdMS_TO_TICKS(200)) == pdTRUE) {

      sent += to_send;

    } else {

      return ESP_ERR_TIMEOUT;
    }
  }

  return ESP_OK;
}

esp_err_t voice_io_spk_stop(void) {

  if (!s_i2s_shared_initialized)

    return ESP_OK;

  /* 播放结束：输出 50ms 静音淡出 → 避免突然切断产生爆破声 */
  if (tx_ringbuf) {
    int16_t silence[400]; /* 16kHz 50ms = 800 采样 */
    for (int i = 0; i < 400; i++)
      silence[i] = 0;
    xRingbufferSend(tx_ringbuf, silence, 400 * 2, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50)); /* 等静音帧播完 */
  }

#if VA_SPK_SD_PIN >= 0

  gpio_set_level(VA_SPK_SD_PIN, 0);

#endif

  /* 【关键】每次 stop 都重置 amp_enabled 标志，
   * 确保下次播放重新跑初始化序列（清 ringbuf + 静音填充） */
  s_amp_enabled = false;

  // 【重要】扔掉手里攥着的半个脏字节，防止污染下一次播放

  s_has_leftover = false;

  s_leftover_byte = 0;

  // 清空 Ringbuffer 残余数据
  if (tx_ringbuf) {
    size_t size;
    void *item;
    while ((item = xRingbufferReceive(tx_ringbuf, &size, 0)) != NULL) {
      vRingbufferReturnItem(tx_ringbuf, item);
    }
  }

  ESP_LOGI(TAG, "Speaker stopped, buffers flushed");

  return ESP_OK;
}