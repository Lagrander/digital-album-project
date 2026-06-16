// lv_voice_assistant.h
#ifndef LV_VOICE_ASSISTANT_H
#define LV_VOICE_ASSISTANT_H

#include "lvgl.h"

/* LLM UI语音助手状态机 */
typedef enum {
    LV_VA_STATE_IDLE,
    LV_VA_STATE_WAKE,
    LV_VA_STATE_LISTEN,
    LV_VA_STATE_RECOGNIZE,
    LV_VA_STATE_THINK,
    LV_VA_STATE_REPLY,
    LV_VA_STATE_EXIT
} LvVaState;

/* 初始化语音助手 UI（创建 Layer3） */
void lv_va_init(void);

/* 显示对话框并进入指定状态 */
void lv_va_show_state(LvVaState state);

/* 隐藏并销毁对话框 */
void lv_va_hide_dialog(void);

/* 本地唤醒词检测初始化（占位） */
void lv_va_wake_detect_init(void);

/* 发送文本到云端并获取回复（占位） */
void lv_va_send_text_to_cloud(const char *text);

/* 显示真实对话文本（用户语音识别结果 + AI 回复） */
void lv_va_show_text(const char *user_text, const char *ai_text, const char *emotion);

#endif // LV_VOICE_ASSISTANT_H
