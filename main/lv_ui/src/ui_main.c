/**
 * @file ui_main.c
 * @brief LVGL 核心视图与视觉生命周期引擎
 *
 * @architecture
 * 本文件负责整个极客电子相册的 UI 渲染生命周期，包括：
 * 1. 跨屏双缓冲视觉架构：搭载 LVGL 8.x，支持平滑的 Crossfade 渐切与动态照片渲染。
 * 2. 多线程资源保护：使用 FreeRTOS 互斥量严格保护所有的屏幕 API 渲染调用，防止 Core 0 的外设与网络流争抢导致总线崩溃。
 * 3. 动态状态栏组件：包含传感器驱动的数据推送与状态机刷新（网络状态、温度等）。
 */
#include "ui_main.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lv_voice_assistant.h"
#include "lvgl.h"
#include "photo_client.h"
#include "ui_icons.h"
#include "ui_png_images.h"
#include "waveshare_rgb_lcd_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "ui_main";

extern const lv_font_t lv_font_cjk_16;
extern const lv_font_t lv_font_cjk_20;
extern const lv_font_t lv_font_montserrat_14;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t lv_font_montserrat_28;

static lv_font_t ui_font_primary_safe; // 顶栏小字号中英文混合字库
static lv_font_t ui_font_large_safe;   // 底栏大字号中英文混合字库
static void safe_cleanup_loading(void);

#define UI_FONT_PRIMARY (&ui_font_primary_safe) /* 顶栏全局使用 */
#define UI_FONT_LARGE (&ui_font_large_safe)     /* 底栏全局使用 */
#define UI_FONT_CAPTION (&ui_font_large_safe)

/* 屏幕逻辑分辨率（竖屏） */
#define LCD_W 480
#define LCD_H 800

/* ── 互斥锁与传感器缓存 ─────────────────────────────────── */
/**
 * @brief UI 渲染互斥锁引用的状态缓存
 *
 * 涉及 FreeRTOS 任务间通信机制。背景网络任务运行在 Core 0，而 LVGL 渲染
 * 运行在 Core 1，因此访问 LVGL 对象或共享状态必须持锁，防止数据竞争。
 */
static float latest_temp = 0.0f;  /* 最新采样温度 */
static float latest_hum = 0.0f;   /* 最新采样湿度 */
static lv_obj_t *img_temp = NULL; /* Layer 0.5：用于 Crossfade 的临时覆盖图 */

/* ── LVGL UI 基础控件句柄引用 ────────────────────────────────────── */
static lv_obj_t *img_main;   /* Layer 0：全屏照片背景图像控件 */
static lv_obj_t *top_bar;    /* Layer 1：顶部状态栏容器（支持懒加载随图显现） */
static lv_obj_t *time_label; /* Layer 1：顶部状态栏 - NTP 时间日期显示文本 */
static lv_obj_t *temp_label; /* Layer 1：顶部状态栏 - DHT20 实时温度显示文本 */
static lv_obj_t *hum_label;  /* Layer 1：顶部状态栏 - DHT20 实时湿度显示文本 */
static lv_obj_t
    *caption_container; /* Layer 2：底部半透明圆角文案容器（支持空状态隐藏） */
static lv_obj_t
    *caption_label; /* Layer 2：底部容器 - AI 大模型相册文案滚动文本 */
static lv_obj_t
    *photo_info_label; /* Layer 2：底部容器 - 照片捕获时间与上传地文本 */
static lv_obj_t *photo_img = NULL; /* 底部相机指示图标 */
static lv_obj_t *upload_banner; /* Layer 3：顶部流式实时照片上传信息通知横幅 */
static lv_obj_t
    *aroma_leds[3]; /* Layer 1：顶部状态栏 - 香薰/雾化各通道指示灯集 */
static lv_obj_t *history_popup = NULL; /* Layer 4：全屏历史记录详情弹窗容器 */
static lv_obj_t *loading_label;        
static lv_timer_t *loading_timer;      

/**
 * @brief LVGL 动态图像格式描述符
 *
 * 声明为 static 全局结构体，以确保在整个显示渲染周期内
 * 传给 lv_img_set_src 的图像元数据在内存中物理常驻且有效。
 */
static lv_img_dsc_t current_img_dsc;

/* 历史记录 */
#define MAX_HISTORY 100

typedef struct {
  char path[128];
  char date[32];
  char caption[256];
} history_entry_t;

static history_entry_t history[MAX_HISTORY];
static int history_count = 0;

/**
 * @brief 记录一条历史照片信息
 *
 * 在多线程环境下由外部任务调用。需配合互斥锁保证安全。
 * 将传入的路径、日期、描述信息复制进历史记录队列中。
 *
 * @param path    照片在 Flash 或 SPIFFS 中的物理存储路径
 * @param date    照片同步日期文本
 * @param caption 照片对应的 AI 文案
 * @return void
 */
void ui_add_history(const char *path, const char *date, const char *caption) {
  if (history_count < MAX_HISTORY) {
    strncpy(history[history_count].path, path,
            sizeof(history[history_count].path) - 1);
    strncpy(history[history_count].date, date,
            sizeof(history[history_count].date) - 1);
    strncpy(history[history_count].caption, caption,
            sizeof(history[history_count].caption) - 1);
    history_count++;
  }
}

/* ── 内部图片刷新机制 ─────────────────────────────────────────── */
/**
 * @brief 在已持锁状态下更新照片
 *
 * 内部辅助函数，外部调用者必须确保已获取 LVGL 互斥锁。
 *
 * @param data    图像数据指针
 * @param caption 图像对应文案
 * @param city    图像城市信息
 * @param date    图像日期信息
 * @return void
 */
static void _set_photo_locked(uint8_t *data, const char *caption,
                              const char *city, const char *date) {
  if (data == NULL) {
    ESP_LOGE(TAG, "图片数据为空，放弃显示");
    return;
  }
  memset(&current_img_dsc, 0, sizeof(current_img_dsc));
  current_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  current_img_dsc.header.w = LCD_W;
  current_img_dsc.header.h = LCD_H;
  current_img_dsc.data_size = LCD_W * LCD_H * 2;
  current_img_dsc.data = data;

  if (img_main) {
    lv_img_set_src(img_main, &current_img_dsc);
  }
  if (top_bar) {
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_HIDDEN);
  }
  if (caption_container) {
    lv_obj_clear_flag(caption_container, LV_OBJ_FLAG_HIDDEN);
  }

  safe_cleanup_loading();

  if (caption_label && caption) {
    lv_label_set_text(caption_label, caption);
  }
  if (photo_info_label && date) {
    // 1.
    
    // '-'
    char cleaned_date[64];
    strncpy(cleaned_date, date, sizeof(cleaned_date) - 1);
    cleaned_date[sizeof(cleaned_date) - 1] = '\0';
    for (int i = 0; cleaned_date[i] != '\0'; i++) {
      if (cleaned_date[i] == '\'' || cleaned_date[i] == '.' ||
          cleaned_date[i] == '/') {
        cleaned_date[i] = '-';
      }
    }

    // 2. 检查城市/地点合法性（防 portrait 等非合法方框数据）
    bool has_valid_city = false;
    if (city && strlen(city) > 0 && strcmp(city, "portrait") != 0 &&
        strcmp(city, "unknown") != 0) {
      has_valid_city = true;
    }

    // 3. 智能联动相机指示图标，拼接自适应显示信息
    char buf[128];
    if (photo_img) {
      lv_obj_clear_flag(photo_img, LV_OBJ_FLAG_HIDDEN); // 始终展现相机小图标
    }

    if (has_valid_city) {
      snprintf(buf, sizeof(buf), "%s  |  %s", cleaned_date,
               city); // "日期  |  地点" 居中呈现
    } else {
      snprintf(buf, sizeof(buf), "%s", cleaned_date); // "日期" 独占一行
    }
    lv_label_set_text(photo_info_label, buf);
  }
}

/* ── 网络层与渲染层交接 ─────────────────────────────────────────── */
/**
 * @brief 将下载的照片数据提交给 UI 渲染
 *
 * 由 photo_client 任务调用，该函数内部会将数据交接给 LVGL，
 * 执行时将锁定 LVGL 互斥量，实现任务间的安全交互。
 *
 * @param rgb565_data 图像 RGB565 数据指针
 * @param len         点阵数据长度
 * @param caption     AI 文案
 * @param city        照片城市信息
 * @param date        照片拍摄日期
 * @return void
 */
void ui_set_photo_data(uint8_t *rgb565_data, size_t len, const char *caption,
                       const char *city, const char *date) {
  _set_photo_locked(rgb565_data, caption, city, date);
}

/* ── 汇编段符号映射引用 ────────────────────────────────────────── */
extern const uint8_t
    placeholder_rgb565_start[] asm("_binary_placeholder_rgb565_start");
extern const uint8_t
    placeholder_rgb565_end[] asm("_binary_placeholder_rgb565_end");

extern const uint8_t
    placeholder_landscape_rgb565_start[] asm("_binary_placeholder_landscape_rgb565_start");
extern const uint8_t
    placeholder_landscape_rgb565_end[] asm("_binary_placeholder_landscape_rgb565_end");

/**
 * @brief 显示相册的空状态占位图
 *
 * 当设备未联网或照片列表为空时调用。
 * 占位图使用编译期间链接到 Flash 中的静态数据，避免运行时的堆内存开销。
 *
 * @return void
 */
void ui_show_placeholder(void) {
  if (img_main) {
    lv_disp_t *disp = lv_disp_get_default();
    bool is_portrait = (disp && lv_disp_get_rotation(disp) == LV_DISP_ROT_90);
    
    memset(&current_img_dsc, 0, sizeof(current_img_dsc));
    current_img_dsc.header.always_zero = 0;
    current_img_dsc.header.w = is_portrait ? 480 : 800;
    current_img_dsc.header.h = is_portrait ? 800 : 480;
    current_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    
    if (is_portrait) {
        current_img_dsc.data_size = (size_t)(placeholder_rgb565_end - placeholder_rgb565_start);
        current_img_dsc.data = placeholder_rgb565_start;
    } else {
        current_img_dsc.data_size = (size_t)(placeholder_landscape_rgb565_end - placeholder_landscape_rgb565_start);
        current_img_dsc.data = placeholder_landscape_rgb565_start;
    }

    lv_img_set_src(img_main, &current_img_dsc);
  }
  if (top_bar) {
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_HIDDEN);
  }
  safe_cleanup_loading();
  if (caption_container) {
    lv_obj_add_flag(caption_container, LV_OBJ_FLAG_HIDDEN);
  }
  if (caption_label) {
    lv_label_set_text(caption_label, " ");
  }
  if (photo_info_label) {
    lv_label_set_text(photo_info_label, " ");
  }
}

/* ── 上传照片通知 ─────────────────────────────────────────── */
/**
 * @brief 处理上传照片通知并在顶部弹出横幅
 *
 * 刷新底层背景图，并加载带有滚动指示文本的通知横幅。
 *
 * @param rgb565_data 照片的原始点阵数据指针
 * @param len         照片点阵大小
 * @param message     上传者附带的消息
 * @param uploader    上传者标识
 * @return void
 */

/* ── 状态更新接口 ─────────────────────────────────────────── */
/**
 * @brief 更新顶部状态栏的温湿度信息
 *
 * 被外设后台任务循环调用。此函数在操作 LVGL 控件前需确保已获取 LVGL 互斥锁。
 *
 * @param temp 本地传感器采样的温度
 * @param hum  本地传感器采样的湿度
 * @return void
 */
void ui_update_weather(float temp, float hum) {
  latest_temp = temp;
  latest_hum = hum;
  if (temp_label) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f°C", temp);
    lv_label_set_text(temp_label, buf);
  }
  if (hum_label) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", hum);
    lv_label_set_text(hum_label, buf);
  }
}

/**
 * @brief 更新顶部状态栏的 NTP 时间
 *
 * 由 UI 内部定时器周期性回调，通过 localtime 检查是否已与 SNTP 同步。
 *
 * @return void
 */
void ui_update_time(void) {
  if (time_label) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[64];
    if (timeinfo.tm_year > (2000 - 1900)) { // 已经过 SNTP 校准
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
               timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
               timeinfo.tm_hour, timeinfo.tm_min);
    } else { // 尚未校准
      snprintf(buf, sizeof(buf), "----- -- - ----:--");
    }
    lv_label_set_text(time_label, buf);
  }
}

/**
 * @brief 更新香薰通道的 UI 状态指示灯
 *
 * @param ch1 通道 1 状态
 * @param ch2 通道 2 状态
 * @param ch3 通道 3 状态
 * @return void
 */
void ui_update_aroma_status(bool ch1, bool ch2, bool ch3) {
  bool status[3] = {ch1, ch2, ch3};
  for (int i = 0; i < 3; i++) {
    if (aroma_leds[i]) {
      if (status[i]) {
        lv_img_set_src(aroma_leds[i], &ui_img_led_green); // 3D绿色打开
      } else {
        lv_img_set_src(aroma_leds[i], &ui_img_led_red); // 3D红色关闭
      }
    }
  }
}

/* 历史记录弹窗 */
/**
 * @brief 关闭历史记录弹窗回调
 *
 * @param e LVGL 点击事件结构体指针
 * @return void
 */
static void close_history_popup_cb(lv_event_t *e) {
  if (history_popup) {
    lv_obj_del(history_popup);
    history_popup = NULL;
  }
}

/**
 * @brief 显示历史记录弹窗
 *
 * 创建全屏半透明遮罩与居中的内容容器，动态渲染历史记录列表。
 *
 * @return void
 */
void ui_show_history_popup(void) {
  if (history_popup) {
    lv_obj_del(history_popup);
    history_popup = NULL;
  }

  /* 全屏半透明遮罩 */
  history_popup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(history_popup, lv_pct(100), lv_pct(100));
  lv_obj_align(history_popup, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(history_popup, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(history_popup, 200, 0);
  lv_obj_set_style_border_width(history_popup, 0, 0);

  /* 内容容器 */
  lv_obj_t *content = lv_obj_create(history_popup);
  lv_obj_set_size(content, lv_pct(88), lv_pct(82));
  lv_obj_align(content, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(content, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_radius(content, 14, 0);
  lv_obj_set_style_border_width(content, 0, 0);

  lv_obj_t *title = lv_label_create(content);
  lv_label_set_text(title, "历史记录");
  lv_obj_set_style_text_font(title, UI_FONT_PRIMARY, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *list = lv_obj_create(content);
  lv_obj_set_size(list, lv_pct(90), lv_pct(78));
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x222222), 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);

  if (history_count == 0) {
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, "暂无历史记录");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(empty, UI_FONT_PRIMARY, 0);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
  } else {
    for (int i = history_count - 1; i >= 0; i--) {
      lv_obj_t *row = lv_obj_create(list);
      lv_obj_set_size(row, lv_pct(100), 44);
      lv_obj_set_style_bg_color(row, lv_color_hex(0x333333), 0);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_radius(row, 6, 0);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

      lv_obj_t *date_l = lv_label_create(row);
      lv_label_set_text(date_l, history[i].date);
      lv_obj_set_style_text_color(date_l, lv_color_hex(0xAAAAAA), 0);
      lv_obj_set_style_text_font(date_l, UI_FONT_PRIMARY, 0);
      lv_obj_set_width(date_l, 90);

      lv_obj_t *cap_l = lv_label_create(row);
      char short_cap[64];
      strncpy(short_cap, history[i].caption, sizeof(short_cap) - 1);
      short_cap[sizeof(short_cap) - 1] = '\0';
      lv_label_set_text(cap_l, short_cap);
      lv_obj_set_style_text_color(cap_l, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_font(cap_l, UI_FONT_PRIMARY, 0);
      lv_obj_set_flex_grow(cap_l, 1);
    }
  }

  /* 关闭按钮 */
  lv_obj_t *close_btn = lv_btn_create(content);
  lv_obj_set_size(close_btn, 70, 34);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_t *close_l = lv_label_create(close_btn);
  lv_label_set_text(close_l, "关闭");
  lv_obj_set_style_text_font(close_l, UI_FONT_PRIMARY, 0);
  lv_obj_center(close_l);
  lv_obj_add_event_cb(close_btn, close_history_popup_cb, LV_EVENT_CLICKED,
                      NULL);
}

/* UI 导航接口 */
void ui_refresh(void) {}
void ui_image_next(void) {}
void ui_image_prev(void) {}

void ui_set_screen_rotation(bool is_portrait) {
  if (lvgl_port_lock(100)) {
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
      lv_disp_set_rotation(disp,
                           is_portrait ? LV_DISP_ROT_90 : LV_DISP_ROT_NONE);
      lvgl_port_set_rotation(is_portrait ? 90 : 0);
    }
    
    // 如果当前显示的是占位图，切换横竖屏底图
    if (img_main && current_img_dsc.data != NULL) {
        if (current_img_dsc.data == placeholder_rgb565_start || current_img_dsc.data == placeholder_landscape_rgb565_start) {
            ui_show_placeholder();
        }
    }
    
    // 自适应底部文案框比例
    if (caption_container) {
        if (is_portrait) {
            lv_obj_set_size(caption_container, lv_pct(92), 64);
            lv_obj_align(caption_container, LV_ALIGN_BOTTOM_MID, 0, -8);
        } else {
            lv_obj_set_size(caption_container, lv_pct(98), 48);
            lv_obj_align(caption_container, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }

    lvgl_port_unlock();
  }
}

/* 开机动画定时器 */
static void loading_timer_cb(lv_timer_t *t) {
  static int dot_count = 0;
  dot_count = (dot_count % 3) + 1;
  char buf[32];

  if (dot_count == 1)
    snprintf(buf, sizeof(buf), "LOADING.");
  else if (dot_count == 2)
    snprintf(buf, sizeof(buf), "LOADING..");
  else
    snprintf(buf, sizeof(buf), "LOADING...");

  if (loading_label) {
    lvgl_port_lock(0);
    lv_label_set_text(loading_label, buf);
    lvgl_port_unlock();
  }
}
static void safe_cleanup_loading(void) {
  if (loading_timer) {
    lv_timer_pause(loading_timer);
    lv_timer_del(loading_timer);
    loading_timer = NULL;
  }
  if (loading_label) {
    lvgl_port_lock(0);
    lv_obj_del(loading_label);
    lvgl_port_unlock();
    loading_label = NULL;
  }
}

/* 主界面初始化 */
/**
 * @brief 初始化 LVGL 主界面布局
 *
 * 按照竖屏 480x800 构建核心图层：背景图层、顶部状态栏、底部文案框等。
 *
 * @return void
 */
void ui_main_init(void) {
  /* 挂载中文字库 */
  memcpy(&ui_font_primary_safe, &lv_font_montserrat_14, sizeof(lv_font_t));
  ui_font_primary_safe.fallback = &lv_font_cjk_16;

  memcpy(&ui_font_large_safe, &lv_font_montserrat_20, sizeof(lv_font_t));
  ui_font_large_safe.fallback = &lv_font_cjk_20;

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF),
                            0); /* 纯白背景与硬件启动特性无缝衔接 */
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  /* ── Layer 0（最底层）：全屏沉浸照片背景（消除硬黑边） ── */
  img_main = lv_img_create(scr);
  lv_obj_set_size(img_main, lv_pct(100), lv_pct(100)); /* 铺满全屏 480x800 */
  lv_obj_align(img_main, LV_ALIGN_TOP_LEFT, 0, 0);     /* 起始于顶角 (0, 0) */
  lv_obj_set_style_bg_color(img_main, lv_color_hex(0x111111), 0);
  lv_obj_move_background(img_main);

  /* ── Layer 1：悬浮半透明顶部信息盖板 ── */
  top_bar = lv_obj_create(scr);
  lv_obj_add_flag(
      top_bar,
      LV_OBJ_FLAG_HIDDEN); /* 上电初期默认隐藏，待图片就绪后再随图显现 */
  lv_obj_set_size(top_bar, lv_pct(100),
                  34); /* 高度设定为 34px，保证状态显示更富呼吸感与对称 */
  lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x1A1C1E), 0); /* 极暗色底 */
  lv_obj_set_style_bg_opa(top_bar, 100,
                          0); /* 约 40% 的高雅半透明，悬浮于照片之上 */
  lv_obj_set_style_border_width(top_bar, 0, 0);
  lv_obj_set_style_radius(top_bar, 0, 0);
  lv_obj_set_style_pad_left(top_bar, 12,
                            0); /* 左右边距对称 12px，不紧贴屏幕边缘 */
  lv_obj_set_style_pad_right(top_bar, 12, 0);
  lv_obj_set_style_pad_top(top_bar, 0, 0); /* 上下 padding 对称清零 */
  lv_obj_set_style_pad_bottom(top_bar, 0, 0);
  lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

  /* 设置 Flex 布局，两端对齐 */
  lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  /* 左对齐区域：温湿度 */
  lv_obj_t *weather_row = lv_obj_create(top_bar);
  lv_obj_set_size(weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(weather_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weather_row, 0, 0);
  lv_obj_set_style_pad_all(weather_row, 0, 0);
  lv_obj_set_height(weather_row, 24); /* 高度设定为 24px */
  lv_obj_set_flex_flow(weather_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(weather_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(weather_row, 4,
                              0); /* 统一设置 Flex 水平列间距为 4px */

  lv_obj_t *temp_img = lv_img_create(weather_row);
  lv_img_set_src(temp_img, &ui_img_temp);

  temp_label = lv_label_create(weather_row);
  lv_label_set_text(temp_label, "--°C");
  lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(temp_label, UI_FONT_PRIMARY, 0);

  lv_obj_t *hum_img = lv_img_create(weather_row);
  lv_img_set_src(hum_img, &ui_img_hum);

  hum_label = lv_label_create(weather_row);
  lv_label_set_text(hum_label, "--%");
  lv_obj_set_style_text_color(hum_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(hum_label, UI_FONT_PRIMARY, 0);

  /* 居中区域：时间日期 */
  lv_obj_t *time_row = lv_obj_create(top_bar);
  lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(time_row, 0, 0);
  lv_obj_set_style_pad_all(time_row, 0, 0);
  lv_obj_set_height(time_row, 24); /* 与温湿度行保持一致 */
  lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(time_row, 5,
                              0); /* 统一设置 Flex 水平间距为 5px */

  lv_obj_t *time_img = lv_img_create(time_row);
  lv_img_set_src(time_img, &ui_img_calendar);

  time_label = lv_label_create(time_row);
  lv_label_set_text(time_label, "----.--.--  --:--");
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(time_label, UI_FONT_PRIMARY, 0);

  /* 右对齐区域：香薰和LED指示灯 */
  lv_obj_t *aroma_row = lv_obj_create(top_bar);
  lv_obj_set_size(aroma_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(aroma_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(aroma_row, 0, 0);
  lv_obj_set_style_pad_all(aroma_row, 0, 0);
  lv_obj_set_height(aroma_row, 24); /* 与其它行对齐 */
  lv_obj_set_flex_flow(aroma_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(aroma_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(aroma_row, 4,
                              0); /* 统一设置 Flex 水平间距为 4px */

  lv_obj_t *aroma_img = lv_img_create(aroma_row);
  lv_img_set_src(aroma_img, &ui_img_fragrance);

  for (int i = 0; i < 3; i++) {
    aroma_leds[i] = lv_img_create(aroma_row);
    lv_img_set_src(aroma_leds[i], &ui_img_led_red); /* 默认 3D 红色关闭 */
  }

  /* ── Layer 2：沉浸深色半透明底栏卡片（锁死为 55px 贴底不加高） ── */
  caption_container = lv_obj_create(scr);
  lv_obj_add_flag(caption_container,
                  LV_OBJ_FLAG_HIDDEN); /* 默认隐藏，待图片就绪后再随照片显现 */
  lv_obj_set_size(caption_container, lv_pct(92), 64); /* 浮动卡片样式 */
  lv_obj_align(caption_container, LV_ALIGN_BOTTOM_MID, 0, -8); /* 抬离底部 */
  lv_obj_set_style_bg_color(caption_container, lv_color_hex(0x1A1C1E), 0);
  lv_obj_set_style_bg_opa(caption_container, 160, 0); /* 更不透明的卡片 */
  lv_obj_set_style_border_width(caption_container, 0, 0);
  lv_obj_set_style_radius(caption_container, 10, 0); /* 圆角浮层 */
  lv_obj_set_style_shadow_width(caption_container, 0, 0);
  lv_obj_set_style_pad_all(caption_container, 12, 0);
  lv_obj_clear_flag(caption_container, LV_OBJ_FLAG_SCROLLABLE);

  /* 纵向 Flex 布局 */
  lv_obj_set_flex_flow(caption_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(caption_container, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(caption_container, 1, 0); /* 缩窄行距至 1px */

  /* 第一行：AI大模型文案，静态折行，纯白色，居中 */
  caption_label = lv_label_create(caption_container);
  lv_obj_set_width(caption_label, lv_pct(100));
  lv_label_set_text(caption_label, "");
  lv_label_set_long_mode(caption_label,
                         LV_LABEL_LONG_WRAP); /* 静态自动折行以完整呈现 */
  lv_obj_set_style_text_color(
      caption_label, lv_color_hex(0xFFFFFF),
      0); /* 纯白色高亮，保证在深色半透明玻璃底色上极其醒目 */
  lv_obj_set_style_text_font(caption_label, UI_FONT_LARGE, 0);
  lv_obj_set_style_text_align(caption_label, LV_TEXT_ALIGN_CENTER,
                              0); 
  lv_obj_set_style_pad_left(caption_label, 4, 0);

  /* 第二行：相机信息行 */
  lv_obj_t *info_row = lv_obj_create(caption_container);
  lv_obj_set_size(info_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(info_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(info_row, 0, 0);
  lv_obj_set_style_outline_width(info_row, 0,
                                 0); /* 强力清零默认外轮廓以粉碎黑框 Bug */
  lv_obj_set_style_shadow_width(info_row, 0,
                                0); /* 强力清零默认投影以粉碎黑框 Bug */
  lv_obj_set_style_pad_all(info_row, 0, 0);
  lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  photo_img = lv_img_create(info_row);
  lv_img_set_src(photo_img, &ui_img_camera);
  lv_obj_set_style_pad_right(photo_img, 6, 0);
  
  lv_obj_set_style_img_recolor(photo_img, lv_color_hex(0x7F8C8D), 0);
  lv_obj_set_style_img_recolor_opa(photo_img, LV_OPA_COVER, 0);

  photo_info_label = lv_label_create(info_row);
  lv_label_set_text(photo_info_label, "");
  lv_obj_set_style_text_color(photo_info_label, lv_color_hex(0x7F8C8D),
                              0); 
  lv_obj_set_style_text_font(photo_info_label, UI_FONT_LARGE, 0);

  /* upload_banner 懒创建（首次调用 ui_show_upload时初始化） */
  upload_banner = NULL;

  
  loading_label = lv_label_create(scr);
  lv_label_set_text(loading_label, "LOADING.");
  lv_obj_set_style_text_color(loading_label, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(loading_label, &lv_font_montserrat_28, 0);
  lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0); 

  loading_timer = lv_timer_create(loading_timer_cb, 500,
                                  NULL); 

  ESP_LOGI(TAG, "UI initialized — new layout %d×%d", LCD_W, LCD_H);
  lv_va_init();
}

/* ── 手机上传照片静态变量与状态维护 ─────────────────────────────────── */
static lv_obj_t *upload_overlay = NULL;
static lv_obj_t *upload_modal = NULL;
static lv_timer_t *upload_timer = NULL;
static uint8_t *new_photo_rgb565 = NULL;
static size_t new_photo_len = 0;
static char upload_message[256];
static char upload_uploader[64];

// 声明外部的旧内存回收函数，在渐变动画完成后被触发
extern void lvgl_ui_release_old_photo(void);

/* ── 动画兼容性包装函数 (针对高级编译器严苛类型安全检查) ── */
static void anim_set_x_cb(void *var, int32_t v) {
  lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_y_cb(void *var, int32_t v) {
  lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_opa_cb(void *var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_set_bg_opa_cb(void *var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/* ── 粒子爆破与动效辅助函数 ─────────────────────────────── */
static void delete_obj_anim_ready_cb(lv_anim_t *a) {
  if (a->var) {
    lv_obj_del(a->var);
  }
}

// 临时顶层图像淡出完成后的回调，销毁临时图像并回收旧图片缓冲区
static void old_img_fade_ready_cb(lv_anim_t *a) {
  if (a->var) {
    lv_obj_del(a->var);
  }
  img_temp = NULL; // 渐变完成，置空临时覆盖图像指针
  lvgl_ui_release_old_photo();
}

static void spawn_particles(lv_obj_t *parent) {
  static const lv_color_t colors[] = {
      {.full = 0xF86B}, // #FF6B6B
      {.full = 0xFED3}, // #FFD93D
      {.full = 0x6E57}, // #6BCB77
      {.full = 0x4CBF}, // #4D96FF
      {.full = 0xFBC2}  // #F78812
  };
  int color_count = sizeof(colors) / sizeof(colors[0]);

  for (int i = 0; i < 4; i++) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 8, 8);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_bg_color(p, colors[rand() % color_count], 0);

    // 居中起始于屏幕正中心 (240, 400)
    lv_obj_align(p, LV_ALIGN_CENTER, 0, 0);

    int angle = rand() % 360;
    int dist = 100 + (rand() % 120); // 放射距离 100 - 220 像素

    // 利用 LVGL 内建高精度三角函数计算放射偏移终点
    int32_t target_x = (lv_trigo_cos(angle) * dist) >> 15;
    int32_t target_y = (lv_trigo_sin(angle) * dist) >> 15;

    // X位移
    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, p);
    lv_anim_set_values(&ax, 0, target_x);
    lv_anim_set_time(&ax, 500);
    lv_anim_set_exec_cb(&ax,
                        anim_set_x_cb); // 完全对应签名，彻底废除强制类型转换！
    lv_anim_start(&ax);

    // Y位移
    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, p);
    lv_anim_set_values(&ay, 0, target_y);
    lv_anim_set_time(&ay, 500);
    lv_anim_set_exec_cb(&ay,
                        anim_set_y_cb); // 完全对应签名，彻底废除强制类型转换！
    lv_anim_start(&ay);

    // 透明度渐隐并自毁
    lv_anim_t ao;
    lv_anim_init(&ao);
    lv_anim_set_var(&ao, p);
    lv_anim_set_values(&ao, 255, 0);
    lv_anim_set_time(&ao, 500);
    lv_anim_set_exec_cb(&ao, anim_set_opa_cb);
    lv_anim_set_ready_cb(&ao, delete_obj_anim_ready_cb);
    lv_anim_start(&ao);
  }
}

static void spawn_particles_timer_cb(lv_timer_t *t) {
  lv_obj_t *parent = (lv_obj_t *)t->user_data;
  if (parent) {
    spawn_particles(parent);
  }
  lv_timer_del(t);
}

static void fade_in_widget(lv_obj_t *obj, uint32_t delay) {
  lv_obj_set_style_opa(obj, 0, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, 450);
  lv_anim_set_delay(&a, delay);
  lv_anim_set_exec_cb(&a,
                      anim_set_opa_cb); // 完全对应签名，彻底废除强制类型转换！
  lv_anim_start(&a);
}


static void dismiss_overlay_ready_cb(lv_anim_t *a) {
  if (a->var) {
    lv_obj_del(a->var); // 销毁遮罩及弹窗子系统
  }
  upload_overlay = NULL;
  upload_modal = NULL;
}

static void upload_dismiss_timer_cb(lv_timer_t *t) {
  (void)t;
  // 1. 退场动画：遮罩与弹窗淡出 500ms
  if (upload_overlay) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, upload_overlay);
    lv_anim_set_values(&a, 153, 0);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(
        &a, anim_set_bg_opa_cb); // 完全对应签名，彻底废除强制类型转换！
    lv_anim_start(&a);
  }
  if (upload_modal) {
    lv_anim_t am;
    lv_anim_init(&am);
    lv_anim_set_var(&am, upload_modal);
    lv_anim_set_values(&am, 0, 600); /* 往下滑出屏幕之外，实现 Y 轴零内存退场 */
    lv_anim_set_time(&am, 500);
    lv_anim_set_exec_cb(&am,
                        anim_set_y_cb); /* 作用于 Y 轴坐标，免遭 OOM 威胁 */
    lv_anim_set_ready_cb(&am, dismiss_overlay_ready_cb);
    lv_anim_start(&am);
  }

  // 2. 照片无缝跨图渐变 (Crossfade)
  if (new_photo_rgb565 && img_main) {
    if (current_img_dsc.data != NULL) {
      // 有旧图片时：执行的渐隐 Crossfade
      static lv_img_dsc_t old_img_dsc;
      memcpy(&old_img_dsc, &current_img_dsc, sizeof(lv_img_dsc_t));

      // 创建临时顶层覆盖图用于过渡
      img_temp = lv_img_create(lv_scr_act());
      lv_obj_set_size(img_temp, LCD_W, LCD_H);         /* 全屏 */
      lv_obj_align(img_temp, LV_ALIGN_TOP_LEFT, 0, 0); /* 起始于顶角 (0, 0) */
      lv_img_set_src(img_temp, &old_img_dsc);
      lv_obj_move_foreground(img_temp);

      // 确保状态栏和底部信息栏位于最顶层
      if (top_bar)
        lv_obj_move_foreground(top_bar);
      if (caption_container)
        lv_obj_move_foreground(caption_container);

      // 升级底层真实主图为新上传照片
      _set_photo_locked(new_photo_rgb565, upload_message, upload_uploader,
                        "云端上传");

      // 顶层覆盖图像在 500ms 内淡出，并自动触发销毁与旧内存释放
      lv_anim_t ax;
      lv_anim_init(&ax);
      lv_anim_set_var(&ax, img_temp);
      lv_anim_set_values(&ax, 255, 0);
      lv_anim_set_time(&ax, 500);
      lv_anim_set_exec_cb(
          &ax, anim_set_opa_cb); // 完全对应签名，彻底废除强制类型转换！
      lv_anim_set_ready_cb(&ax, old_img_fade_ready_cb);
      lv_anim_start(&ax);
    } else {
      // 未发现旧图，直接加载新照片
      _set_photo_locked(new_photo_rgb565, upload_message, upload_uploader,
                        "手机上传");
      // 此时直接安全释放老照片缓冲区
      lvgl_ui_release_old_photo();
    }
  }
}

bool ui_is_upload_animating(void) {
  return (upload_overlay != NULL || img_temp != NULL);
}

void ui_force_dismiss_upload(void) {
  // 1. 如果还在展示弹窗阶段：立即销毁弹窗与遮罩，安全回收上一张旧图的内存
  if (upload_overlay) {
    lv_obj_del(upload_overlay);
    upload_overlay = NULL;
    upload_modal = NULL;
    lvgl_ui_release_old_photo();
  }

  // 2. 如果已经进入 Crossfade 阶段：立即强行销毁淡出的临时顶层图，安全回收旧图
  if (img_temp) {
    lv_obj_del(img_temp);
    img_temp = NULL;
    lvgl_ui_release_old_photo();
  }

  // 3. 删除未到期的单次欣赏定时器
  if (upload_timer) {
    lv_timer_del(upload_timer);
    upload_timer = NULL;
  }
}

/* ── 手机上传接口主体实现 ───────────────────────────────── */
void ui_show_upload(uint8_t *rgb565_data, size_t len, const char *message,
                    const char *uploader) {
  if (!rgb565_data || len != 768000) {
    ESP_LOGE(TAG, "ui_show_upload: Invalid parameters. data=%p, len=%d",
             rgb565_data, len);
    return;
  }

  // 终止之前的动画并释放内存
  ui_force_dismiss_upload();

  // A. 缓存新照片数据及留言信息
  new_photo_rgb565 = rgb565_data;
  new_photo_len = len;
  if (message) {
    strncpy(upload_message, message, sizeof(upload_message) - 1);
    upload_message[sizeof(upload_message) - 1] = '\0';
  } else {
    strcpy(upload_message, "静享美好时光~");
  }
  if (uploader) {
    strncpy(upload_uploader, uploader, sizeof(upload_uploader) - 1);
    upload_uploader[sizeof(upload_uploader) - 1] = '\0';
  } else {
    strcpy(upload_uploader, "神秘人");
  }

  // B. Step 1: 创建全屏沉浸式黑色降噪遮罩
  upload_overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(upload_overlay, lv_pct(100), lv_pct(100));
  lv_obj_align(upload_overlay, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(upload_overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(upload_overlay, 0, 0);
  lv_obj_set_style_border_width(upload_overlay, 0, 0);
  lv_obj_set_style_radius(upload_overlay, 0, 0);
  lv_obj_clear_flag(upload_overlay, LV_OBJ_FLAG_SCROLLABLE);

  // 黑色背景淡入 400ms 至 60% 透明度 (153)
  lv_anim_t a_overlay;
  lv_anim_init(&a_overlay);
  lv_anim_set_var(&a_overlay, upload_overlay);
  lv_anim_set_values(&a_overlay, 0, 153);
  lv_anim_set_time(&a_overlay, 400);
  lv_anim_set_exec_cb(
      &a_overlay,
      anim_set_bg_opa_cb); // 完全对应签名，彻底废除强制类型转换！
  lv_anim_start(&a_overlay);

  // B. 创建模态框弹窗
  upload_modal = lv_obj_create(upload_overlay);
  lv_obj_set_size(upload_modal, 360, 480);
  lv_obj_align(upload_modal, LV_ALIGN_CENTER, 0, 480);
  lv_obj_set_style_bg_color(upload_modal, lv_color_hex(0xFDF5E6), 0);
  lv_obj_set_style_radius(upload_modal, 16, 0);
  lv_obj_set_style_border_width(upload_modal, 0, 0);
  lv_obj_set_style_pad_all(upload_modal, 20, 0);
  lv_obj_clear_flag(upload_modal, LV_OBJ_FLAG_SCROLLABLE);

  // 绘制边框
  lv_obj_set_style_shadow_width(upload_modal, 0, 0);
  lv_obj_set_style_border_color(upload_modal, lv_color_hex(0xE67E22), 0);
  lv_obj_set_style_border_width(upload_modal, 2, 0);

  lv_obj_set_flex_flow(upload_modal, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(upload_modal, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // 滑入动画
  lv_anim_t a_modal;
  lv_anim_init(&a_modal);
  lv_anim_set_var(&a_modal, upload_modal);
  lv_anim_set_values(&a_modal, 480, 0);
  lv_anim_set_time(&a_modal, 600);
  lv_anim_set_path_cb(&a_modal, lv_anim_path_overshoot);
  lv_anim_set_exec_cb(&a_modal, anim_set_y_cb);
  lv_anim_start(&a_modal);

  // D. 弹窗内部件排版与淡入设计
  // D1. 橙色精美圆圈信箱图标
  lv_obj_t *icon_circle = lv_obj_create(upload_modal);
  lv_obj_set_size(icon_circle, 80, 80);
  lv_obj_set_style_radius(icon_circle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(icon_circle, lv_color_hex(0xE67E22),
                            0); // 暖意橙色
  lv_obj_set_style_border_width(icon_circle, 0, 0);
  lv_obj_set_style_pad_all(icon_circle, 0, 0);
  lv_obj_clear_flag(icon_circle, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *icon_label = lv_label_create(icon_circle);
  lv_label_set_text(icon_label, "NEW");
  lv_obj_set_style_text_font(icon_label, UI_FONT_PRIMARY, 0);
  lv_obj_set_style_text_color(icon_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(icon_label);

  // D2. 上传照片标题
  lv_obj_t *title_label = lv_label_create(upload_modal);
  lv_label_set_text(title_label, "收到云端投递！");
  lv_obj_set_style_text_font(title_label, UI_FONT_PRIMARY, 0);
  lv_obj_set_style_text_color(title_label, lv_color_hex(0x1A1A1A), 0);

  // D3. 发送者署名
  lv_obj_t *sender_label = lv_label_create(upload_modal);
  char s_buf[128];
  snprintf(s_buf, sizeof(s_buf), "发件人: %s", upload_uploader);
  lv_label_set_text(sender_label, s_buf);
  lv_obj_set_style_text_font(sender_label, UI_FONT_PRIMARY, 0);
  lv_obj_set_style_text_color(sender_label, lv_color_hex(0x7F8C8D), 0);

  // D4. 分割横线
  lv_obj_t *div_line = lv_obj_create(upload_modal);
  lv_obj_set_size(div_line, 280, 2);
  lv_obj_set_style_bg_color(div_line, lv_color_hex(0xBDC3C7), 0);
  lv_obj_set_style_border_width(div_line, 0, 0);

  // D5. 纸张信件质感留言文本区
  lv_obj_t *msg_container = lv_obj_create(upload_modal);
  lv_obj_set_size(msg_container, 320, 140);
  lv_obj_set_style_bg_color(msg_container, lv_color_hex(0xF5EBD6), 0);
  lv_obj_set_style_radius(msg_container, 8, 0);
  lv_obj_set_style_border_width(msg_container, 0, 0);
  lv_obj_set_style_pad_all(msg_container, 12, 0);
  lv_obj_clear_flag(msg_container, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *msg_label = lv_label_create(msg_container);
  lv_obj_set_width(msg_label, lv_pct(100));
  lv_label_set_long_mode(msg_label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(msg_label, upload_message);
  lv_obj_set_style_text_font(msg_label, UI_FONT_PRIMARY, 0);
  lv_obj_set_style_text_color(msg_label, lv_color_hex(0x2C3E50), 0);
  lv_obj_align(msg_label, LV_ALIGN_TOP_LEFT, 0, 0);

  // E. Step 4: 渐进式延迟阶梯淡入，赋予绝佳空间节奏感
  fade_in_widget(icon_circle, 200);
  fade_in_widget(title_label, 200);
  fade_in_widget(sender_label, 350);
  fade_in_widget(msg_container, 500);

  // F. Step 3: 在 200ms 弹窗展开就绪的瞬间，爆发彩色放射粒子
  lv_timer_create(spawn_particles_timer_cb, 200, upload_overlay);

  // G. Step 5: 启动 3.8 秒欣赏与渐变退场单次定时器
  if (upload_timer) {
    lv_timer_del(upload_timer);
  }
  upload_timer = lv_timer_create(upload_dismiss_timer_cb, 3800, NULL);
}