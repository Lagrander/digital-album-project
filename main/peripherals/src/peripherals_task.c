/**
 * @file peripherals_task.c
 * @brief 外围设备任务控制逻辑。
 *
 * 管理温湿度传感器与香薰雾化器的定期轮询与状态同步。
 */
#include "peripherals_task.h"
#include "aht20_sensor.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "he30.h"
#include "mpu6050_sensor.h"
#include "pcf8574_io.h"
#include "tca9548a.h"
#include "voice_io.h"
#include <stdio.h>

/**
 * @brief 外设任务主循环
 *
 * 负责初始化各种 I2C 传感器与 I/O 扩展器，并在死循环中周期性
 * 读取温湿度及同步雾化器状态。
 *
 * @param param 任务参数
 */
void app_peripherals(void *param) {
  (void)param;
  i2c_bus_mutex_init();

  if (i2c_sensor_aht20_init() != ESP_OK) {
    ESP_LOGE("peripherals", "AHT20 init failed!");
  }
  if (pcf8574_io_init() != ESP_OK) {
    ESP_LOGE("peripherals", "PCF8574 init failed!");
  }

  // 初始化 MPU6050
  if (mpu6050_init() != ESP_OK) {
    ESP_LOGE("peripherals", "MPU6050 init failed!");
  }

  // 初始化 3 个雾化器句柄，配置相关引脚
  he30_init_all(EXT_IO_PIN_0, EXT_IO_PIN_1, EXT_IO_PIN_2);

  while (1) {
    // 同步雾化器硬件与目标状态
    he30_sync();

    peripheral_aht20();

    peripheral_mpu6050();

    vTaskDelay(pdMS_TO_TICKS(300)); // 轮询周期缩短到 300ms
  }

  vTaskDelete(NULL);
}

static void uart_ctrl_task(void *arg) {
  static bool t_state[3] = {false, false, false};
  while (1) {
    int c = getchar();
    if (c != EOF) {
      switch (c) {
      case '1':
        t_state[0] = !t_state[0]; // Toggle
        he30_set_target(0, t_state[0]);
        ESP_LOGI("uart_ctrl", "Channel 1 %s", t_state[0] ? "ON" : "OFF");
        break;
      case '2':
        t_state[1] = !t_state[1]; // Toggle
        he30_set_target(1, t_state[1]);
        ESP_LOGI("uart_ctrl", "Channel 2 %s", t_state[1] ? "ON" : "OFF");
        break;
      case '3':
        t_state[2] = !t_state[2]; // Toggle
        he30_set_target(2, t_state[2]);
        ESP_LOGI("uart_ctrl", "Channel 3 %s", t_state[2] ? "ON" : "OFF");
        break;
      case '4': {
        // Toggle ALL
        bool new_state = !(t_state[0] && t_state[1] && t_state[2]);
        t_state[0] = t_state[1] = t_state[2] = new_state;
        he30_set_target(0, new_state);
        he30_set_target(1, new_state);
        he30_set_target(2, new_state);
        ESP_LOGI("uart_ctrl", "All Channels %s", new_state ? "ON" : "OFF");
        break;
      }
      case '5': {
        ESP_LOGI("uart_ctrl", "Testing Audio (1s Beep 440Hz)");
        static bool audio_inited = false;
        if (!audio_inited) {
          esp_err_t err = voice_io_spk_init(16000, 1, 16);
          if (err == ESP_OK) {
            audio_inited = true;
          } else {
            ESP_LOGE("uart_ctrl", "Audio init failed, cannot play");
            break;
          }
        }
        int16_t *beep = malloc(32000); // 16000 samples = 1s
        if (beep) {
          for (int i = 0; i < 16000; i++) {
            // 16000Hz / 440Hz ≈ 36 samples per period (18 high, 18 low)
            beep[i] = ((i / 18) % 2 == 0) ? 4000 : -4000;
          }

          ESP_LOGI("uart_ctrl", "Printing the first 36 samples of the audio "
                                "wave (1 full period):");
          for (int i = 0; i < 36; i++) {
            printf("%d ", beep[i]);
          }
          printf("\n");
          ESP_LOGI("uart_ctrl", "Now sending INFINITE continuous wave to I2S DMA. (Reboot to stop)...");

          while (1) {
             voice_io_spk_play_stream((const uint8_t *)beep, 32000);
          }
          // The code below is unreachable now, but kept for completeness
          voice_io_spk_stop();
          free(beep);
        } else {
          ESP_LOGE("uart_ctrl", "Failed to allocate memory for beep");
        }
        break;
      }
      case ' ':
        t_state[0] = t_state[1] = t_state[2] = false;
        he30_set_target(0, false);
        he30_set_target(1, false);
        he30_set_target(2, false);
        ESP_LOGI("uart_ctrl", "All Channels OFF");
        break;
      default:
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

/**
 * @brief 初始化外设任务
 *
 * 静态创建并拉起外设轮询任务。
 */
void app_peripherals_init(void) {
  static StaticTask_t task_tcb;
  static StackType_t task_stack[1024 * 4];

  TaskHandle_t task_peripherals =
      xTaskCreateStatic(app_peripherals, "peripherals_task", 1024 * 4, NULL, 1,
                        task_stack, /* 数组 */
                        &task_tcb   /*  TCB */
      );

  if (task_peripherals == NULL) {
    printf("Failed to create peripherals task\n");
  }

  // 创建串口控制任务
  xTaskCreate(uart_ctrl_task, "uart_ctrl_task", 1024 * 2, NULL, 1, NULL);
}

/* ── 雾化器控制接口（线程安全）────────────────────────── */

/**
 * @brief 开启全部香薰/雾化通道
 */
void peripherals_mist_on(void) {
  for (int i = 0; i < 3; i++) {
    he30_set_target(i, true);
  }
  ESP_LOGI("peripherals", "Voice → Mist ON (all 3 channels)");
}

/**
 * @brief 关闭全部香薰/雾化通道
 */
void peripherals_mist_off(void) {
  for (int i = 0; i < 3; i++) {
    he30_set_target(i, false);
  }
  ESP_LOGI("peripherals", "Voice → Mist OFF (all 3 channels)");
}

/**
 * @brief 查询是否有任意一路雾化器处于开启状态
 *
 * @return true 至少有一路开启
 * @return false 全部关闭
 */
bool peripherals_mist_is_on(void) {
  for (int i = 0; i < 3; i++) {
    if (he30_get_state(i))
      return true;
  }
  return false;
}
