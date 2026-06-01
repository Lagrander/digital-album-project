#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wi-Fi 管理器：
 * - STA 模式连接 + 自动重连
 * - 连接失败时启动 AP 配置门户
 * - NVS 存储凭据
 */

esp_err_t net_mgr_init(void);
esp_err_t net_mgr_wait_connected(int timeout_ms);
bool net_mgr_is_connected(void);
void net_mgr_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
