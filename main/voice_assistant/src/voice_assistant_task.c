/**
 * @file voice_assistant_task.c
 * @brief 本地唤醒与语音助手任务总控
 *
 * @architecture
 * 整合 ESP-SR 的 WakeNet 与音频管线：
 * 1. 负责 INMP441 麦克风的唤醒词侦测。
 * 2. 唤醒后挂起主循环，启动基于 WebSocket 的流式 ASR/LLM 对话机制。
 */
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
#include "../../lv_ui/src/lv_voice_assistant.h"
#include "peripherals_task.h"
#include "sdkconfig.h"
#include "voice_io.h"
#include "stream_player.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

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
    case VA_CMD_VOL_UP: {
        uint8_t vol = voice_io_get_spk_volume();
        if (vol <= 90) vol += 10; else vol = 100;
        voice_io_set_spk_volume(vol);
        ESP_LOGI(TAG, "Voice → volume up: %d%%", vol);
        break;
    }
    case VA_CMD_VOL_DOWN: {
        uint8_t vol = voice_io_get_spk_volume();
        if (vol >= 10) vol -= 10; else vol = 0;
        voice_io_set_spk_volume(vol);
        ESP_LOGI(TAG, "Voice → volume down: %d%%", vol);
        break;
    }
    case VA_CMD_BRIGHT_UP:
        ESP_LOGI(TAG, "Voice → brightness up");
        ui_voice_brightness_up();
        break;
    case VA_CMD_BRIGHT_DOWN:
        ESP_LOGI(TAG, "Voice → brightness down");
        ui_voice_brightness_down();
        break;
    case VA_CMD_MIST_LEVEL_UP:
        ESP_LOGI(TAG, "Voice → mist level up");
        peripherals_mist_level_up();
        break;
    case VA_CMD_MIST_LEVEL_DOWN:
        ESP_LOGI(TAG, "Voice → mist level down");
        peripherals_mist_level_down();
        break;
    case VA_CMD_AUDIO_PLAY: {
        char url[128];
        snprintf(url, sizeof(url), "%s/music/test.mp3", CONFIG_SERVER_URL);
        ESP_LOGI(TAG, "Voice → audio play: %s", url);
        stream_player_play_url(url);
        break;
    }
    case VA_CMD_AUDIO_STOP:
        ESP_LOGI(TAG, "Voice → audio stop");
        stream_player_stop();
        break;
    case VA_CMD_MODE_SLEEP:
        ESP_LOGI(TAG, "Voice → sleep mode");
        ui_voice_sleep();
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
    if (ui_is_sleep_mode()) {
        ui_voice_wake_from_sleep();
    }
    ui_voice_on_wake();
    lv_va_show_state(LV_VA_STATE_WAKE);
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
    // 映射 voice_assistant 状态机 → LVGL VA UI 状态
    // 0=ST_WAITING_WAKEUP  1=ST_RECORDING  2=ST_WAITING_RESP
    switch (state) {
    case 0: /* 等待唤醒 → IDLE，UI 自动 EXIT */ break;
    case 1: lv_va_show_state(LV_VA_STATE_LISTEN); break;
    case 2: lv_va_show_state(LV_VA_STATE_THINK); break;
    }
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
        .det_mode  = 0,                 /* 0 代表 DET_MODE_90，避免唤醒阈值过高导致无法触发 */
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

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(app_voice_assistant, "voice", 1024 * 8,
                                                 NULL, 5, task_stack, &task_tcb, 1);
    if (!h) {
        ESP_LOGE(TAG, "Failed to create voice assistant task");
    }
}