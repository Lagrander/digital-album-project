#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "peripherals.h"
#include "aht20_sensor.h"
#include "pcf8574_io.h"
#include "he30.h"
#include "esp_log.h"
#include <stdio.h>



void app_peripherals(void *param)
{
    (void)param;
    if (i2c_sensor_aht20_init() != ESP_OK) {
        ESP_LOGE("peripherals", "AHT20 init failed!");
    }
    if (pcf8574_io_init() != ESP_OK) {
        ESP_LOGE("peripherals", "PCF8574 init failed!");
    }
    
    // 2. 创建 3 个雾化器句柄
    // 假设使用低电平开启 (active_low=true) 配合外部上拉电阻
    he30_init_all(EXT_IO_PIN_0, EXT_IO_PIN_1, EXT_IO_PIN_2);


    while (1)
    {
        // 核心：调用同步函数，自动匹配硬件与目标状态
        he30_sync();

        peripheral_aht20();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelete(NULL);
}

void app_peripherals_init(void)
{
    static StaticTask_t task_tcb;
    static StackType_t task_stack[1024 * 4];


    TaskHandle_t task_peripherals = xTaskCreateStatic(
        app_peripherals,
        "peripherals_task",
        1024 * 4,
        NULL, 1,
        task_stack,   /* 数组 */
        &task_tcb     /*  TCB */
    );

    if(task_peripherals == NULL) {
        printf("Failed to create peripherals task\n");
    }
}
