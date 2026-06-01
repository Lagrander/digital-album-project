/*
 * voice_assistant_task.c — 语音助手任务包装实现
 *
 * 负责:
 *   1. 实现 va_callbacks_t 回调（连接语音命令到 UI / 外设）
 *   2. 提供 app_voice_assistant() 任务入口
 *   3. 提供 app_voice_assistant_init() 创建 FreeRTOS 任务
 *
 * 回调路由:
 *   VA_CMD_NEXT_PHOTO / VA_CMD_PREV_PHOTO → ui_voice_next_photo / ui_voice_prev_photo
 *   VA_CMD_MIST_ON    / VA_CMD_MIST_OFF    → peripherals_mist_on / peripherals_mist_off
 *   VA_CMD_BYE                              → 已在 voice_assistant.c 内部处理
 *   va_on_wake                              → ui_voice_on_wake
 *   va_on_state_change                      → ui_voice_on_state
 *
 * 线程安全: UI 回调内部持有 LVGL 锁，外设回调内部持有互斥锁。
 */

#include <stdio.h>
#include "voice_assistant_task.h"
#include "voice_assistant.h"
#include "lvgl_ui_task.h"
#include "peripherals_task.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "voice_task";

/* ── 语音命令回调 ──────────────────────────────────────────── */

/*
 * 本地命令识别回调。
 *
 * 由 voice_assistant.c 的 handle_local_cmd() 在 MultiNet7 检测到
 * 自定义命令词时调用。根据 cmd_id 路由到对应模块:
 *   - 照片操作 → UI 模块（ui_voice_next_photo / ui_voice_prev_photo）
 *   - 雾化控制 → 外设模块（peripherals_mist_on / peripherals_mist_off）
 *
 * @param cmd_id 命令 ID（VA_CMD_* 枚举值）
 * @param ctx    用户上下文（未使用）
 */
static void va_on_command(int cmd_id, void *ctx)
{
    (void)ctx;
    switch (cmd_id) {
    case VA_CMD_NEXT_PHOTO:
        ESP_LOGI(TAG, "Voice → next photo");
        ui_voice_next_photo();
        break;
    case VA_CMD_PREV_PHOTO:
        ESP_LOGI(TAG, "Voice → prev photo");
        ui_voice_prev_photo();
        break;
    case VA_CMD_MIST_ON:
        ESP_LOGI(TAG, "Voice → mist on");
        peripherals_mist_on();
        break;
    case VA_CMD_MIST_OFF:
        ESP_LOGI(TAG, "Voice → mist off");
        peripherals_mist_off();
        break;
    default:
        break;
    }
}

/*
 * 唤醒词检测回调。
 * WakeNet9 检测到 "你好小智" 后由语音助手调用。
 * 通知 UI 显示唤醒指示（如 LED 闪烁、图标动画等）。
 *
 * @param ctx 用户上下文（未使用）
 */
static void va_on_wake(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Wake word detected!");
    ui_voice_on_wake();
}

/*
 * LLM 语音回复播放完毕回调。
 * 当前仅用于日志追踪，后续可扩展为更新 UI 状态。
 *
 * @param ctx 用户上下文（未使用）
 */
static void va_on_response_done(void *ctx)
{
    (void)ctx;
    /* LLM 语音回复播放完毕，可在此更新 UI */
}

/*
 * 状态机状态变化回调。
 * 将状态变更通知 UI，用于显示当前模式（等待唤醒 / 录音中 / LLM 回复中）。
 *
 * @param state 新状态: 0=等待唤醒, 1=录音中, 2=等待 LLM 回复
 * @param ctx   用户上下文（未使用）
 */
static void va_on_state_change(int state, void *ctx)
{
    (void)ctx;
    ui_voice_on_state(state);
}

/* ── 任务入口 ───────────────────────────────────────────────── */

/*
 * 语音助手任务主函数。
 *
 * 执行流程:
 *   1. 延时 2 秒等待 LCD + LVGL 初始化完成
 *   2. 构建 va_config_t（WS URI 来自 Kconfig，灵敏度 90）
 *   3. 构建 va_callbacks_t（绑定以上静态回调函数）
 *   4. 调用 va_init() 初始化 ESP-SR + WebSocket
 *   5. 调用 va_run() 进入主循环，永不返回
 *
 * @param param 未使用（FreeRTOS 任务签名要求）
 */
void app_voice_assistant(void *param)
{
    (void)param;

    /* 等待 UI 任务先完成初始化（LCD + LVGL 就绪） */
    vTaskDelay(pdMS_TO_TICKS(2000));

    va_config_t cfg = {
        .ws_uri    = CONFIG_VA_WS_URI,  /* Kconfig 中配置的语音服务器地址 */
        .wake_word = NULL,              /* NULL = 使用默认唤醒词 "nihaoxiaozhi" */
        .det_mode  = 90,                /* 唤醒灵敏度: 80(灵敏) / 90(均衡) / 95(严格) */
    };

    va_callbacks_t cbs = {
        .on_command       = va_on_command,
        .on_wake          = va_on_wake,
        .on_response_done = va_on_response_done,
        .on_state_change  = va_on_state_change,
        .ctx              = NULL,
    };

    ESP_LOGI(TAG, "Starting voice assistant...");
    esp_err_t ret = va_init(&cfg, &cbs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "va_init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    va_run();   /* 永不返回 */
}

/* ── 任务创建 ───────────────────────────────────────────────── */

/*
 * 创建语音助手 FreeRTOS 任务。
 *
 * 任务参数:
 *   - 栈大小: 8 KB（ESP-SR 模型推理 + WebSocket 回调 + 状态机需要较大栈）
 *   - 优先级: 5（中等优先级，低于 UI 任务避免影响画面刷新）
 *   - 分配方式: 静态分配（避免动态内存碎片）
 *
 * 调用时机: 应在 main.c 中 UI + 外设初始化完成后调用。
 */
void app_voice_assistant_init(void)
{
    static StaticTask_t task_tcb;
    static StackType_t task_stack[1024 * 8];

    TaskHandle_t h = xTaskCreateStatic(app_voice_assistant, "voice", 1024 * 8,
                                       NULL, 5, task_stack, &task_tcb);
    if (!h) {
        ESP_LOGE(TAG, "Failed to create voice assistant task");
    }
}
