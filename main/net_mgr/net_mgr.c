/**
 * @file net_mgr.c
 * @brief 网络连接管理模块。
 *
 * 负责 Wi-Fi STA 模式初始化、连接与事件回调，并提供查询 IP 等网络状态的方法。
 */
#include "net_mgr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "net_mgr";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
static const int WIFI_MAX_RETRY = 10;
static esp_netif_t *s_sta_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *ev =
        (wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", ev->reason);
    if (s_retry_num < WIFI_MAX_RETRY) {
      esp_wifi_connect();
      s_retry_num++;
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
    s_retry_num = 0;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

/**
 * @brief 初始化网络管理器
 *
 * 初始化 NVS 闪存、创建事件循环组、注册 Wi-Fi 与 IP 获取事件并开始连接配置好的 AP。
 *
 * @return esp_err_t 返回 ESP_OK 则初始化成功
 */
esp_err_t net_mgr_init(void) {
  // NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK)
    return ret;

  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  cfg.nvs_enable = false; // 禁用 Wi-Fi 自动擦写 NVS，防止写入失败触发 Cache stall 锁死总线
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD,
          sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  // 关闭 Wi-Fi 省电模式，确保实时 WebSocket 数据流的高吞吐与极低延迟
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_LOGI(TAG, "Wi-Fi STA init done, SSID=%s", CONFIG_WIFI_SSID);
  return ESP_OK;
}

/**
 * @brief 阻塞等待 Wi-Fi 连接成功
 *
 * @param timeout_ms 最大等待超时时间（毫秒）
 * @return esp_err_t 返回 ESP_OK 连接成功，返回 ESP_ERR_TIMEOUT 超时，或其它失败状态
 */
esp_err_t net_mgr_wait_connected(int timeout_ms) {
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      pdMS_TO_TICKS(timeout_ms));

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Wi-Fi connected");
    return ESP_OK;
  }
  if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Wi-Fi connection failed after retries");
    return ESP_FAIL;
  }
  ESP_LOGE(TAG, "Wi-Fi connection timeout");
  return ESP_ERR_TIMEOUT;
}

/**
 * @brief 检查当前是否已获取到 IP 并处于连接状态
 *
 * @return true 
 * @return false 
 */
bool net_mgr_is_connected(void) {
  EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
  return (bits & WIFI_CONNECTED_BIT) != 0;
}

/**
 * @brief 获取当前设备的 IP 地址字符串
 *
 * @param buf 指向输出缓冲区的指针
 * @param len 缓冲区长度
 */
void net_mgr_get_ip(char *buf, size_t len) {
  esp_netif_t *netif = s_sta_netif;
  if (!netif) {
    snprintf(buf, len, "N/A");
    return;
  }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
    snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
  } else {
    snprintf(buf, len, "N/A");
  }
}
