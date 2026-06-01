#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PHOTOS_PER_DAY 10
#define MAX_SIDE_LEN 64
#define MAX_CITY_LEN 64
#define MAX_MESSAGE_LEN 128

typedef struct {
  char id[64];
  char date[16];
  char side[MAX_SIDE_LEN];
  char caption[256];
  float memory;
  char city[MAX_CITY_LEN];
  float lat;
  float lon;
} photo_metadata_t;

typedef struct {
  bool has_upload;
  char id[64];
  char message[MAX_MESSAGE_LEN];
  char uploader_name[64];
  bool downloaded;
} upload_info_t;

/**
 * 获取当日照片列表
 * @return ESP_OK 成功
 */
esp_err_t photo_client_fetch_today(photo_metadata_t *photos, int *count);

/**
 * 下载照片 RGB565 数据到 PSRAM
 * @param photo_id  照片 ID（从 metadata 获得）
 * @param buf_out   输出：PSRAM 缓冲区指针
 * @param len_out   输出：字节长度（应为 800*480*2 = 768000）
 */
esp_err_t photo_client_download_rgb565(const char *photo_id, uint8_t **buf_out,
                                       size_t *len_out);

/**
 * 查询当日是否有惊喜照片
 */
esp_err_t photo_client_check_upload(upload_info_t *info);

/**
 * 下载惊喜照片 RGB565 数据到 PSRAM
 */
esp_err_t photo_client_download_upload(const char *upload_id, uint8_t **buf_out,
                                       size_t *len_out);

/**
 * 释放照片缓冲区
 */
void photo_client_free_buf(uint8_t *buf);

typedef struct {
    bool has_command;
    char cmd[32];
    char target_id[64];
    int channel;
    int state;
} device_command_t;

/**
 * 发送设备心跳并上报状态
 */
esp_err_t photo_client_send_heartbeat(const char *current_photo_id, float fps, uint32_t free_mem, int aroma[3]);

/**
 * 获取待处理的遥控指令
 */
esp_err_t photo_client_fetch_command(device_command_t *cmd);

/**
 * 设置服务器 URL（可在运行时覆盖 Kconfig 配置）
 */
void photo_client_set_server_url(const char *url);
const char *photo_client_get_server_url(void);

#ifdef __cplusplus
}
#endif
