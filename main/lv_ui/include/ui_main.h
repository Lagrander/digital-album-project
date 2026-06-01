#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "lvgl.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_main_init(void);
void ui_update_weather(float temp, float hum);
void ui_update_time(void);
void ui_update_aroma_status(bool ch1, bool ch2, bool ch3);
void ui_refresh(void);

/** 动态旋转屏幕方向 */
void ui_set_screen_rotation(bool is_portrait);
void ui_image_next(void);
void ui_image_prev(void);

/** 设置主图 RGB565 数据（由 photo_client 下载后调用） */
void ui_set_photo_data(uint8_t *rgb565_data, size_t len, const char *caption,
                       const char *city, const char *date);

/** 显示占位图 */
void ui_show_placeholder(void);


/**
 * @brief 触发手机/电脑上传照片高级全屏弹窗与照片无缝跨图渐变切换动效
 * 
 * @param rgb565_data 新照片原始点阵数据指针
 * @param len         照片点阵大小
 * @param message     上传者寄语留言
 * @param uploader    上传者姓名/昵称
 */
void ui_show_upload(uint8_t *rgb565_data, size_t len, const char *message, const char *uploader);
void ui_force_dismiss_upload(void);
bool ui_is_upload_animating(void);

/** 添加历史记录 */
void ui_add_history(const char *path, const char *date, const char *caption);

/** 弹出历史记录 */
void ui_show_history_popup(void);

#ifdef __cplusplus
}
#endif

#endif
