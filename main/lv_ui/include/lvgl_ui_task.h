#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LVGL UI 任务入口
 * - LCD 初始化 + UI 框架启动
 * - 照片下载调度 + 惊喜检查
 * - 传感器 / 时间更新定时器
 */

/** 由 main.c 调用，创建 UI 任务 */
void app_ui_init(void);

/** UI 任务主函数（FreeRTOS） */
void app_ui(void *param);

/* ── 语音助手回调 ────────────────────────────────────────────
 * 由 voice task 调用（非 UI 线程），内部已处理 LVGL 锁。
 */

/** 语音触发切到下一张照片 */
void ui_voice_next_photo(void);

/** 语音触发切到上一张照片 */
void ui_voice_prev_photo(void);

/** 唤醒指示（LED / 图标闪烁等） */
void ui_voice_on_wake(void);

/** 状态变化指示（0=等待唤醒, 1=录音中, 2=等待LLM回复） */
void ui_voice_on_state(int state);

#ifdef __cplusplus
}
#endif
