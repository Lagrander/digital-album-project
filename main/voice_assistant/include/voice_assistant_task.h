/*
 * voice_assistant_task.h — 语音助手任务包装
 *
 * 创建运行语音状态机（唤醒 → 录音 → LLM 对话）的 FreeRTOS 任务。
 * 回调函数将本地命令路由到 UI（照片导航）和外设（雾化控制）。
 *
 * 使用方式:
 *   1. main.c 在 UI + 外设就绪后调用 app_voice_assistant_init()
 *   2. 任务内部延时 2 秒等待 LCD 完成初始化
 *   3. 调用 va_init() + va_run()，永不返回
 *
 * 依赖: voice_assistant.h, lvgl_ui_task.h, peripherals_task.h
 */

#ifndef VOICE_ASSISTANT_TASK_H
#define VOICE_ASSISTANT_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 语音助手任务入口函数。
 * 调用 va_init() 初始化 ESP-SR 模型和 WebSocket，
 * 然后进入 va_run() 主循环，永不返回。
 *
 * @param param 未使用（FreeRTOS 任务签名要求）
 */
void app_voice_assistant(void *param);

/**
 * 创建语音助手 FreeRTOS 任务。
 * 任务栈 8 KB，优先级 5，静态分配。
 * 应在 UI 和外设初始化完成后调用。
 */
void app_voice_assistant_init(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_ASSISTANT_TASK_H */
