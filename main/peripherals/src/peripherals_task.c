/**
 * @file peripherals_task.c
 * @brief 外围设备任务控制逻辑。
 *
 * 管理温湿度传感器与香薰雾化器的定期轮询与状态同步。
 */
#include "peripherals_task.h"
#include "aht20_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "he30.h"
#include "pcf8574_io.h"
#include "mpu6050_sensor.h"
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
  // if (i2c_sensor_aht20_init() != ESP_OK) {
  //   ESP_LOGE("peripherals", "AHT20 init failed!");
  // }
  if (pcf8574_io_init() != ESP_OK) {
    ESP_LOGE("peripherals", "PCF8574 init failed!");
  }

  // 初始化 MPU6050
  if (mpu6050_init() != ESP_OK) {
    ESP_LOGE("peripherals", "MPU6050 init failed!");
  }

  // 初始化 3 个雾化器句柄，配置相关引脚
  // he30_init_all(EXT_IO_PIN_0, EXT_IO_PIN_1, EXT_IO_PIN_2);

  while (1) {
    // 同步雾化器硬件与目标状态
    // he30_sync();

    // peripheral_aht20();
    
    peripheral_mpu6050();

    vTaskDelay(pdMS_TO_TICKS(200)); // 轮询周期缩短到 200ms，大幅提高响应速度
  }

  vTaskDelete(NULL);
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
