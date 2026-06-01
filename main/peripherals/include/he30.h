#ifndef HE30_H
#define HE30_H

#include "esp_err.h"
#include <stdbool.h>
#include "pcf8574_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HE-30 雾化器控制句柄
 */
typedef struct {
    ext_io_pin_t pin;      // 连接在 PCF8574 的哪个引脚 (0~7)
    bool active_low;       // true: 低电平开启 (建议); false: 高电平开启
} he30_handle_t;

// 定义雾化器的状态结构
typedef struct {
    he30_handle_t handle;
    bool is_on;   // 软件记录当前实际开关状态（应与硬件一致）
} he30_control_t;

/**
 * @brief 初始化 HE-30 雾化器（分配引脚和逻辑）
 */
he30_handle_t he30_init(ext_io_pin_t pin, bool active_low);

/**
 * @brief 开启雾化器 (底层操作)
 */
esp_err_t he30_on(he30_handle_t handle);

/**
 * @brief 关闭雾化器 (底层操作)
 */
esp_err_t he30_off(he30_handle_t handle);

/**
 * @brief 批量初始化三个雾化器
 */
void he30_init_all(ext_io_pin_t pin0, ext_io_pin_t pin1, ext_io_pin_t pin2);

/**
 * @brief 获取单个模块的当前实际开关状态（软件记录）
 */
bool he30_get_state(int index);

/**
 * @brief 设置单个模块的开关状态 (直接操作硬件)
 * @note 仅在硬件操作成功后才更新软件状态 `is_on`
 */
esp_err_t he30_set_state(int index, bool on);

/**
 * @brief 设置单个模块的【目标状态】（由 LLM 或其他逻辑调用）
 * @note 此函数仅更新 `target_state`，不会立即操作硬件。
 */
void he30_set_target(int index, bool target);

/**
 * @brief 同步所有模块：检查 `target_state` 与当前实际状态，
 *        若不一致，则调用 `he30_set_state` 进行硬件切换。。
 */
void he30_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* HE30_H */