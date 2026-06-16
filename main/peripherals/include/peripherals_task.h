#ifndef PERIPHERALS_H
#define PERIPHERALS_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 外设管理任务
 * - AHT20 温湿度传感器
 * - HE30 雾化器 ×3（通过 PCF8574 IO 扩展）
 */

/** 外设任务主函数（FreeRTOS） */
void app_peripherals(void *param);

/** 由 main.c 调用，创建外设任务 */
void app_peripherals_init(void);

/* ── 雾化器控制（语音助手回调 / 外部逻辑）──────────────────── */

/** 打开全部 3 路雾化器 */
void peripherals_mist_on(void);

/** 关闭全部 3 路雾化器 */
void peripherals_mist_off(void);

/** 查询雾化器状态（任意一路开则返回 true） */
bool peripherals_mist_is_on(void);

/** 设置指定通道的雾化开关与强度 */
void peripherals_set_aroma(int channel, bool on, int level);

/** 加大当前活跃通道的喷雾强度（1→2→3） */
void peripherals_mist_level_up(void);

/** 减小当前活跃通道的喷雾强度（3→2→1） */
void peripherals_mist_level_down(void);

/** 查询是否因湿度过高被强制关闭 */
bool peripherals_is_humidity_blocked(void);

/** 字符串通道名→索引映射（"mint"→0, "jasmine"→1, "rose"→2） */
int peripherals_mist_channel_from_string(const char *ch_str);

#ifdef __cplusplus
}
#endif

#endif // PERIPHERALS_H
