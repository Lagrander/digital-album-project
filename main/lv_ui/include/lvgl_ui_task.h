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

/** 语音触发亮度+10 */
void ui_voice_brightness_up(void);

/** 语音触发亮度-10 */
void ui_voice_brightness_down(void);

/** 语音触发休眠模式 */
void ui_voice_sleep(void);

/** 休眠唤醒 */
void ui_voice_wake_from_sleep(void);

/** 查询是否处于休眠模式 */
bool ui_is_sleep_mode(void);

/** 云端指令：显示指定 URL 的照片，可设置 hold_mode */
void ui_show_photo_from_url(const char *url, const char *hold_mode);

/** 云端指令：解除锁定，恢复自动轮播 */
void ui_resume_playlist(void);

#ifdef __cplusplus
}
#endif
