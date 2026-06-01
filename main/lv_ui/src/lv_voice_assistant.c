// lv_voice_assistant.c
#include "lv_voice_assistant.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "lv_voice_assistant";

/* 导入中文字体 */
extern const lv_font_t lv_font_cjk_16;

/* 全局 UI 对象 */
static lv_obj_t *assistant_layer = NULL;   // 全屏遮罩层（半透明）
static lv_obj_t *dialog_box = NULL;       // 对话卡片容器

/* 动效与气泡关键组件 */
static lv_obj_t *state_title = NULL;       // 顶部状态文本
static lv_obj_t *feedback_container = NULL; // 核心反馈区容器 (120x120)
static lv_obj_t *mic_icon = NULL;          // 居中麦克风图标/提示
static lv_obj_t *loading_arc = NULL;       // 思考中旋转弧圈
static lv_obj_t *ripples[3] = {NULL};      // 涟漪水波纹圆圈
static lv_obj_t *chat_container = NULL;    // 对话区域容器
static lv_obj_t *user_label = NULL;        // 用户问话文本 label
static lv_obj_t *ai_label = NULL;          // AI 答复文本 label

/* 动效与定时器辅助变量 */
static lv_timer_t *typewriter_timer = NULL;
static lv_timer_t *exit_timer = NULL;
static char typewriter_text[512];
static uint32_t typewriter_index = 0;

/* 前向声明 */
static void lv_va_create_dialog(void);
static void va_stop_ripple_anims(void);
static void va_exit_timer_cb(lv_timer_t *t);

/* ── 动效底层回调函数 ── */
static void va_anim_set_bg_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void va_anim_set_y_cb(void *var, int32_t v) {
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void va_arc_rotate_cb(void *var, int32_t v) {
    if (var) {
        lv_arc_set_rotation((lv_obj_t *)var, v);
    }
}

static void ripple_anim_cb(void *var, int32_t v) {
    lv_obj_t *obj = (lv_obj_t *)var;
    if (!obj) return;
    int32_t size = 24 + (34 * v) / 100;
    lv_obj_set_size(obj, size, size);
    int32_t opa = 200 - (200 * v) / 100;
    lv_obj_set_style_opa(obj, opa, 0);
}

/* ── UTF-8 安全宽度获取 ── */
static uint32_t get_utf8_char_len(const char *s) {
    if ((s[0] & 0x80) == 0) return 1;
    if ((s[0] & 0xE0) == 0xC0) return 2;
    if ((s[0] & 0xF0) == 0xE0) return 3;
    if ((s[0] & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ── 核心动效控制逻辑 ── */
static void va_start_ripple_anims(void) {
    for (int i = 0; i < 3; i++) {
        if (!ripples[i]) {
            ripples[i] = lv_obj_create(feedback_container);
            lv_obj_set_style_radius(ripples[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(ripples[i], 0, 0);
            lv_obj_set_style_border_color(ripples[i], lv_color_hex(0x5A9EFC), 0); // 耀目霓虹蓝
            lv_obj_set_style_border_width(ripples[i], 1, 0);
            lv_obj_align(ripples[i], LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_clear_flag(ripples[i], LV_OBJ_FLAG_HIDDEN);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ripples[i]);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 1200);
        lv_anim_set_delay(&a, i * 400); // 均匀涟漪铺开
        lv_anim_set_exec_cb(&a, ripple_anim_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

static void va_stop_ripple_anims(void) {
    for (int i = 0; i < 3; i++) {
        if (ripples[i]) {
            lv_anim_del(ripples[i], ripple_anim_cb);
            lv_obj_add_flag(ripples[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ── 物理自释放退场回调 ── */
static void va_dismiss_anim_ready_cb(lv_anim_t *a) {
    (void)a;
    if (assistant_layer) {
        lv_obj_del(assistant_layer);
        assistant_layer = NULL;
        dialog_box = NULL;
        state_title = NULL;
        feedback_container = NULL;
        mic_icon = NULL;
        loading_arc = NULL;
        chat_container = NULL;
        user_label = NULL;
        ai_label = NULL;
        for (int i = 0; i < 3; i++) ripples[i] = NULL;
    }
}

/* 创建对话卡片 UI */
static void lv_va_create_dialog(void) {
    lv_obj_t *scr = lv_scr_act();
    assistant_layer = lv_obj_create(scr);
    lv_obj_set_size(assistant_layer, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(assistant_layer, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(assistant_layer, LV_GRAD_DIR_NONE, 0); // 显式关闭默认垂直渐变色以消除灰色雾霾
    lv_obj_set_style_shadow_width(assistant_layer, 0, 0); // 显式消除阴影
    lv_obj_set_style_bg_opa(assistant_layer, 0, 0); // 初始透明，由动画淡入至 180 (70% 暗化)
    lv_obj_set_style_border_width(assistant_layer, 0, 0);
    lv_obj_add_flag(assistant_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(assistant_layer);

    // 对话卡片（居中、圆角、半透明深灰底）
    dialog_box = lv_obj_create(assistant_layer);
    lv_obj_set_size(dialog_box, 192, 160); // 192x160 迷你尺寸
    lv_obj_set_style_bg_color(dialog_box, lv_color_hex(0x1E1E24), 0); // 高雅深太灰色
    lv_obj_set_style_bg_opa(dialog_box, 230, 0); // ~90% 不透明度
    lv_obj_set_style_radius(dialog_box, 16, 0); // 圆角矩形
    lv_obj_set_style_border_width(dialog_box, 0, 0);
    lv_obj_set_style_pad_all(dialog_box, 8, 0); // 缩窄边距
    lv_obj_clear_flag(dialog_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dialog_box, LV_ALIGN_CENTER, 0, 400); // 初始偏下，等待跃入

    // A. 顶部中文状态标签
    state_title = lv_label_create(dialog_box);
    lv_label_set_text(state_title, "🤖 语音助手");
    lv_obj_set_style_text_font(state_title, &lv_font_cjk_16, 0);
    lv_obj_set_style_text_color(state_title, lv_color_hex(0xA0A5B5), 0); // 优雅浅灰色
    lv_obj_align(state_title, LV_ALIGN_TOP_MID, 0, 4); // 微降4px获得更好的视觉呼吸感

    // B. 核心反馈区容器 (60x60)
    feedback_container = lv_obj_create(dialog_box);
    lv_obj_set_size(feedback_container, 60, 60);
    lv_obj_align(feedback_container, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_set_style_bg_opa(feedback_container, 0, 0);
    lv_obj_set_style_border_width(feedback_container, 0, 0);
    lv_obj_clear_flag(feedback_container, LV_OBJ_FLAG_SCROLLABLE);

    // B1. 居中麦克风图标 label
    mic_icon = lv_label_create(feedback_container);
    lv_label_set_text(mic_icon, "🎤");
    lv_obj_set_style_text_font(mic_icon, &lv_font_cjk_16, 0);
    lv_obj_set_style_text_color(mic_icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(mic_icon, LV_ALIGN_CENTER, 0, 0);

    // B2. 科技感旋转 Loading 环
    loading_arc = lv_arc_create(feedback_container);
    lv_obj_set_size(loading_arc, 44, 44); // 缩微加载环
    lv_obj_align(loading_arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_angles(loading_arc, 0, 60); // 60 度单弧
    lv_arc_set_bg_angles(loading_arc, 0, 360);
    lv_obj_set_style_arc_opa(loading_arc, 30, LV_PART_MAIN);
    lv_obj_set_style_arc_color(loading_arc, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_arc_color(loading_arc, lv_color_hex(0x5A9EFC), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(loading_arc, 3, LV_PART_MAIN); // 线宽也稍微减薄
    lv_obj_set_style_arc_width(loading_arc, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(loading_arc, LV_OBJ_FLAG_HIDDEN);

    // C. 文本对话区容器 (Flex Column)
    chat_container = lv_obj_create(dialog_box);
    lv_obj_set_size(chat_container, 180, 56); // 180x56 迷你文本窗
    lv_obj_align(chat_container, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(chat_container, 0, 0);
    lv_obj_set_style_border_width(chat_container, 0, 0);
    lv_obj_set_style_pad_all(chat_container, 0, 0);
    lv_obj_set_flex_flow(chat_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(chat_container, 2, 0); // 紧凑行距 2px
    lv_obj_clear_flag(chat_container, LV_OBJ_FLAG_SCROLLABLE);

    // C1. 用户问话 label
    user_label = lv_label_create(chat_container);
    lv_obj_set_width(user_label, lv_pct(100));
    lv_label_set_long_mode(user_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(user_label, "");
    lv_obj_set_style_text_font(user_label, &lv_font_cjk_16, 0);
    lv_obj_set_style_text_color(user_label, lv_color_hex(0xFECB2F), 0); // 暖亮橙黄色
    lv_obj_add_flag(user_label, LV_OBJ_FLAG_HIDDEN);

    // C2. AI 答复 label
    ai_label = lv_label_create(chat_container);
    lv_obj_set_width(ai_label, lv_pct(100));
    lv_label_set_long_mode(ai_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ai_label, "");
    lv_obj_set_style_text_font(ai_label, &lv_font_cjk_16, 0);
    lv_obj_set_style_text_color(ai_label, lv_color_hex(0xFFFFFF), 0); // 洁白色
    lv_obj_add_flag(ai_label, LV_OBJ_FLAG_HIDDEN);
}

/* 打字机定时器回调 */
static void typewriter_timer_cb(lv_timer_t *t) {
    if (!t || !t->user_data) return;
    lv_obj_t *label = (lv_obj_t *)t->user_data;
    
    if (typewriter_index < strlen(typewriter_text)) {
        uint32_t len = get_utf8_char_len(&typewriter_text[typewriter_index]);
        typewriter_index += len;
        
        char *buf = malloc(typewriter_index + 1);
        if (buf) {
            memcpy(buf, typewriter_text, typewriter_index);
            buf[typewriter_index] = '\0';
            lv_label_set_text(label, buf);
            free(buf);
        }
    } else {
        lv_timer_del(t);
        typewriter_timer = NULL;
        // 打字机完成，启动无持续交互自动退场超时（4秒）
        if (exit_timer) {
            lv_timer_del(exit_timer);
        }
        exit_timer = lv_timer_create(va_exit_timer_cb, 4000, NULL);
    }
}

static void va_exit_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    exit_timer = NULL;
    lv_va_show_state(LV_VA_STATE_EXIT);
}

/* 公共接口：初始化语音助手 UI */
void lv_va_init(void) {
    ESP_LOGI(TAG, "Initializing voice assistant UI placeholder");
    // 本地唤醒词检测初始化（占位）
    lv_va_wake_detect_init();
}

/* 显示对话框并进入指定状态 */
void lv_va_show_state(LvVaState state) {
    // 退出状态外，若图层缺失，立即懒加载动态创建
    if (state != LV_VA_STATE_EXIT && !assistant_layer) {
        lv_va_create_dialog();
    }
    
    // 清除打字机与退场定时器，避免状态重叠
    if (typewriter_timer) { lv_timer_del(typewriter_timer); typewriter_timer = NULL; }
    if (exit_timer) { lv_timer_del(exit_timer); exit_timer = NULL; }

    switch (state) {
        case LV_VA_STATE_WAKE: {
            lv_obj_clear_flag(assistant_layer, LV_OBJ_FLAG_HIDDEN);
            
            // 1. 全屏黑色遮罩淡入至 180 (70%)
            lv_anim_t a_mask;
            lv_anim_init(&a_mask);
            lv_anim_set_var(&a_mask, assistant_layer);
            lv_anim_set_values(&a_mask, 0, 180);
            lv_anim_set_time(&a_mask, 400);
            lv_anim_set_exec_cb(&a_mask, va_anim_set_bg_opa_cb);
            lv_anim_start(&a_mask);
            
            // 2. 卡片阻尼回弹入场 (overshoot) 从 800 -> 320
            lv_anim_t a_card;
            lv_anim_init(&a_card);
            lv_anim_set_var(&a_card, dialog_box);
            lv_anim_set_values(&a_card, 800, 320);
            lv_anim_set_time(&a_card, 550);
            lv_anim_set_path_cb(&a_card, lv_anim_path_overshoot);
            lv_anim_set_exec_cb(&a_card, va_anim_set_y_cb);
            lv_anim_start(&a_card);
            
            lv_label_set_text(state_title, "🤖 语音助手");
            lv_obj_add_flag(loading_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(mic_icon, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case LV_VA_STATE_LISTEN: {
            lv_label_set_text(state_title, "🎤 聆听中...");
            lv_obj_add_flag(loading_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(mic_icon, LV_OBJ_FLAG_HIDDEN);
            
            // 启动霓虹水波纹声波扩散
            va_start_ripple_anims();
            break;
        }
        case LV_VA_STATE_RECOGNIZE: {
            lv_label_set_text(state_title, "🧠 识别中...");
            va_stop_ripple_anims();
            
            // 在此模拟接收到语音文本：为 "打开雾化器"
            lv_obj_clear_flag(user_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(user_label, "你：打开雾化器");
            break;
        }
        case LV_VA_STATE_THINK: {
            lv_label_set_text(state_title, "🤖 思考中...");
            lv_obj_add_flag(mic_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(loading_arc, LV_OBJ_FLAG_HIDDEN);
            
            // 开启加载圈高频旋转动画
            lv_anim_t a_spin;
            lv_anim_init(&a_spin);
            lv_anim_set_var(&a_spin, loading_arc);
            lv_anim_set_values(&a_spin, 0, 360);
            lv_anim_set_time(&a_spin, 1000);
            lv_anim_set_exec_cb(&a_spin, va_arc_rotate_cb);
            lv_anim_set_repeat_count(&a_spin, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a_spin);
            break;
        }
        case LV_VA_STATE_REPLY: {
            lv_label_set_text(state_title, "💬 回复中...");
            lv_anim_del(loading_arc, va_arc_rotate_cb);
            lv_obj_add_flag(loading_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(mic_icon, LV_OBJ_FLAG_HIDDEN); // 复原麦克风为点缀
            
            // 启动打字机输出
            lv_obj_clear_flag(ai_label, LV_OBJ_FLAG_HIDDEN);
            strncpy(typewriter_text, "AI：好的，已为您开启雾化香薰通道。", sizeof(typewriter_text) - 1);
            typewriter_index = 0;
            typewriter_timer = lv_timer_create(typewriter_timer_cb, 50, ai_label);
            break;
        }
        case LV_VA_STATE_EXIT: {
            if (assistant_layer) {
                va_stop_ripple_anims();
                lv_anim_del(loading_arc, va_arc_rotate_cb);
                
                // 1. 遮罩淡出
                lv_anim_t a_mask;
                lv_anim_init(&a_mask);
                lv_anim_set_var(&a_mask, assistant_layer);
                lv_anim_set_values(&a_mask, 180, 0);
                lv_anim_set_time(&a_mask, 400);
                lv_anim_set_exec_cb(&a_mask, va_anim_set_bg_opa_cb);
                lv_anim_start(&a_mask);
                
                // 2. 卡片滑出屏幕 (ease_in) 从 320 -> 800
                lv_anim_t a_card;
                lv_anim_init(&a_card);
                lv_anim_set_var(&a_card, dialog_box);
                lv_anim_set_values(&a_card, 320, 800);
                lv_anim_set_time(&a_card, 400);
                lv_anim_set_path_cb(&a_card, lv_anim_path_ease_in);
                lv_anim_set_exec_cb(&a_card, va_anim_set_y_cb);
                lv_anim_set_ready_cb(&a_card, va_dismiss_anim_ready_cb); // 结束后彻底删除销毁释放堆内存
                lv_anim_start(&a_card);
            }
            break;
        }
        default:
            break;
    }
}

/* 隐藏并销毁对话框 */
void lv_va_hide_dialog(void) {
    lv_va_show_state(LV_VA_STATE_EXIT);
}

/* 本地唤醒词检测初始化（占位实现） */
void lv_va_wake_detect_init(void) {
    ESP_LOGI(TAG, "Wake word detection init (placeholder)");
}

/* 发送文本到云端并获取回复（占位实现） */
void lv_va_send_text_to_cloud(const char *text) {
    ESP_LOGI(TAG, "Sending text to cloud: %s", text);
    lv_va_show_state(LV_VA_STATE_REPLY);
}
