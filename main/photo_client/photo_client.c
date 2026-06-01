/**
 * @file photo_client.c
 * @brief 提供设备与后端服务器进行 HTTP 交互的客户端接口。
 *
 * 负责拉取照片列表、下载图片点阵数据、上报设备心跳以及获取远程遥控指令等功能。
 */
#include "photo_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "photo_client";
static char g_server_url[256] = CONFIG_SERVER_URL;

const char* photo_client_get_server_url(void) {
    return g_server_url;
}

// 竖屏模式：逻辑分辨率 480×800
#define LCD_W 480
#define LCD_H 800
#define RGB565_SIZE (LCD_W * LCD_H * 2)  // 768000 bytes

void photo_client_set_server_url(const char *url)
{
    if (url) {
        strncpy(g_server_url, url, sizeof(g_server_url) - 1);
        g_server_url[sizeof(g_server_url) - 1] = '\0';
    }
}

/**
 * @brief 内部辅助函数：执行 HTTP GET 请求并将响应保存到内存中
 *
 * 自动处理 chunked 编码及大文件内存扩展分配。支持选择分配在内部 RAM 或 PSRAM。
 * 
 * @param url           请求的 URL 地址
 * @param buf_out       用于存放响应数据的内存指针地址的输出参数
 * @param len_out       实际读取到的数据长度的输出参数
 * @param use_psram     是否强制使用 PSRAM 分配内存
 * @param expected_size 预期文件大小，便于预分配连续内存块（若未知可传 0）
 * @return esp_err_t    返回 ESP_OK 成功，或其它错误码
 */
static esp_err_t http_get_to_buf(const char *url, uint8_t **buf_out, size_t *len_out,
                                  bool use_psram, size_t expected_size)
{
    size_t alloc_size = expected_size > 0 ? expected_size : 4096;
    uint8_t *buf = use_psram
        ? heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM)
        : malloc(alloc_size);
    if (!buf) {
        ESP_LOGE(TAG, "malloc(%d) failed", (int)alloc_size);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(buf);
        return err;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len > 0 && (size_t)content_len > alloc_size) {
        uint8_t *new_buf = use_psram
            ? heap_caps_realloc(buf, content_len, MALLOC_CAP_SPIRAM)
            : realloc(buf, content_len);
        if (new_buf) {
            buf = new_buf;
            alloc_size = content_len;
        }
    }

    size_t total = 0;
    int read_len;
    do {
        size_t remaining = alloc_size - total;
        if (remaining < 1024) {
            alloc_size *= 2;
            uint8_t *new_buf = use_psram
                ? heap_caps_realloc(buf, alloc_size, MALLOC_CAP_SPIRAM)
                : realloc(buf, alloc_size);
            if (!new_buf) { err = ESP_ERR_NO_MEM; goto cleanup; }
            buf = new_buf;
            remaining = alloc_size - total;
        }
        read_len = esp_http_client_read(client, (char *)(buf + total), remaining);
        if (read_len > 0) total += read_len;
    } while (read_len > 0);

    if (read_len < 0) err = ESP_FAIL;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *buf_out = buf;
    *len_out = total;
    return ESP_OK;
}

/**
 * @brief 获取当日展示照片列表
 *
 * 从服务端拉取今天对应的待展示照片 JSON 数组，解析其中的关键信息并填充到 photos 数组中。
 * 
 * @param photos 指向用来存储元数据的结构体数组指针
 * @param count  用于输出成功获取的照片数量的指针
 * @return esp_err_t 返回 ESP_OK 获取成功，或其它错误码
 */
esp_err_t photo_client_fetch_today(photo_metadata_t *photos, int *count)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/today", g_server_url);

    uint8_t *json_buf = NULL;
    size_t json_len = 0;
    esp_err_t ret = http_get_to_buf(url, &json_buf, &json_len, false, 0);
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_ParseWithLength((char *)json_buf, (int)json_len);
    free(json_buf);
    if (!root) return ESP_FAIL;

    cJSON *arr = cJSON_GetObjectItem(root, "photos");
    int n = 0;
    if (arr && cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (n >= MAX_PHOTOS_PER_DAY) break;
            photo_metadata_t *p = &photos[n];
            cJSON *id = cJSON_GetObjectItem(item, "id");
            cJSON *date = cJSON_GetObjectItem(item, "date");
            cJSON *side = cJSON_GetObjectItem(item, "side");
            cJSON *caption = cJSON_GetObjectItem(item, "caption");
            cJSON *mem = cJSON_GetObjectItem(item, "memory");
            cJSON *city = cJSON_GetObjectItem(item, "city");
            cJSON *lat = cJSON_GetObjectItem(item, "lat");
            cJSON *lon = cJSON_GetObjectItem(item, "lon");

            if (id && id->valuestring)
                strncpy(p->id, id->valuestring, sizeof(p->id) - 1);
            if (date && date->valuestring)
                strncpy(p->date, date->valuestring, sizeof(p->date) - 1);
            if (side && side->valuestring)
                strncpy(p->side, side->valuestring, sizeof(p->side) - 1);
            if (caption && caption->valuestring)
                strncpy(p->caption, caption->valuestring, sizeof(p->caption) - 1);
            if (city && city->valuestring)
                strncpy(p->city, city->valuestring, sizeof(p->city) - 1);
            p->memory = mem ? (float)mem->valuedouble : 0;
            p->lat = lat ? (float)lat->valuedouble : 0;
            p->lon = lon ? (float)lon->valuedouble : 0;
            n++;
        }
    }
    *count = n;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Fetched %d photos for today", n);
    return ESP_OK;
}

/**
 * @brief 下载对应 ID 的 RGB565 照片点阵数据
 *
 * 考虑到 480x800 分辨率将占用 768000 字节，分配在 PSRAM 中进行下载。
 * 
 * @param photo_id 要下载的照片的唯一标识符
 * @param buf_out  返回下载缓冲区的指针，由调用方负责释放
 * @param len_out  返回下载的总长度
 * @return esp_err_t 返回 ESP_OK 则表示下载完整，其它代表异常
 */
esp_err_t photo_client_download_rgb565(const char *photo_id, uint8_t **buf_out, size_t *len_out)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/photo/%s.rgb565", g_server_url, photo_id);

    uint8_t *buf = heap_caps_malloc(RGB565_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM malloc(%d) failed", RGB565_SIZE);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); heap_caps_free(buf); return err; }

    esp_http_client_fetch_headers(client);

    size_t total = 0;
    int read_len;
    while (total < RGB565_SIZE) {
        read_len = esp_http_client_read(client, (char *)(buf + total), RGB565_SIZE - total);
        if (read_len <= 0) break;
        total += read_len;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total != RGB565_SIZE) {
        ESP_LOGW(TAG, "Download size %d != %d", (int)total, RGB565_SIZE);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    *buf_out = buf;
    *len_out = total;
    ESP_LOGI(TAG, "Downloaded %s (%d bytes)", photo_id, (int)total);
    return ESP_OK;
}

/**
 * @brief 轮询服务端检查是否有手机端新上传的照片
 *
 * 解析服务端 JSON 响应，获取上传照片的基本信息。
 *
 * @param info 用于存放提取的上传信息的结构体指针
 * @return esp_err_t 返回 ESP_OK 则正常完成通讯（不代表有新照片），或返回通讯错误
 */
esp_err_t photo_client_check_upload(upload_info_t *info)
{
    memset(info, 0, sizeof(*info));

    char url[512];
    snprintf(url, sizeof(url), "%s/api/upload/check", g_server_url);

    uint8_t *json_buf = NULL;
    size_t json_len = 0;
    esp_err_t ret = http_get_to_buf(url, &json_buf, &json_len, false, 0);
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_ParseWithLength((char *)json_buf, (int)json_len);
    free(json_buf);
    if (!root) return ESP_FAIL;

    cJSON *has = cJSON_GetObjectItem(root, "has_upload");
    info->has_upload = has ? cJSON_IsTrue(has) : false;

    if (info->has_upload) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        cJSON *name = cJSON_GetObjectItem(root, "uploader_name");
        cJSON *dl = cJSON_GetObjectItem(root, "downloaded");

        if (id && id->valuestring)
            strncpy(info->id, id->valuestring, sizeof(info->id) - 1);
        if (msg && msg->valuestring)
            strncpy(info->message, msg->valuestring, sizeof(info->message) - 1);
        if (name && name->valuestring)
            strncpy(info->uploader_name, name->valuestring, sizeof(info->uploader_name) - 1);
        info->downloaded = dl ? cJSON_IsTrue(dl) : false;
        ESP_LOGI(TAG, "Upload photo found: %s from %s", info->id, info->uploader_name);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 下载用户最新上传的照片 RGB565 点阵数据
 *
 * @param upload_id 上传记录的唯一标识符
 * @param buf_out   返回下载缓冲区的指针（PSRAM），由调用方负责释放
 * @param len_out   返回下载的总长度
 * @return esp_err_t 返回 ESP_OK 下载成功，或其它异常
 */
esp_err_t photo_client_download_upload(const char *upload_id, uint8_t **buf_out, size_t *len_out)
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/upload/%s.rgb565", g_server_url, upload_id);
    // 复用同样的下载逻辑
    // 实际上可以直接调用 photo_client_download_rgb565 类似的逻辑

    uint8_t *buf = heap_caps_malloc(RGB565_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .buffer_size = 8192,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(client); heap_caps_free(buf); return err; }
    esp_http_client_fetch_headers(client);

    size_t total = 0;
    int read_len;
    while (total < RGB565_SIZE) {
        read_len = esp_http_client_read(client, (char *)(buf + total), RGB565_SIZE - total);
        if (read_len <= 0) break;
        total += read_len;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total != RGB565_SIZE) { heap_caps_free(buf); return ESP_FAIL; }
    *buf_out = buf;
    *len_out = total;
    return ESP_OK;
}

/**
 * @brief 上报设备心跳与当前状态至服务端
 *
 * 将当前显示的图片 ID，剩余内存，运行帧率和外设开启状态封装成 JSON 并 POST 提交。
 *
 * @param current_photo_id 当前正在展示的照片 ID
 * @param fps              当前 UI 渲染估算帧率
 * @param free_mem         当前系统剩余堆内存大小
 * @param aroma            各通道雾化器状态（长度为 3 的数组）
 * @return esp_err_t 
 */
esp_err_t photo_client_send_heartbeat(const char *current_photo_id, float fps, uint32_t free_mem, int aroma[3])
{
    char url[512];
    snprintf(url, sizeof(url), "%s/api/device/heartbeat", g_server_url);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "current_photo_id", current_photo_id ? current_photo_id : "");
    cJSON_AddNumberToObject(root, "fps", fps);
    cJSON_AddNumberToObject(root, "free_mem", free_mem);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 3; ++i) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(aroma[i]));
    }
    cJSON_AddItemToObject(root, "aroma_channels", arr);

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return ESP_FAIL;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(post_data); return ESP_FAIL; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Heartbeat failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
    free(post_data);
    return err;
}

/**
 * @brief 从服务端获取排队的设备远程遥控指令
 *
 * 解析服务端的指令下发，如切换照片、控制香薰雾化等。
 *
 * @param cmd_out 承载解析后指令内容的结构体指针
 * @return esp_err_t 返回 ESP_OK 则成功解析，否则返回异常
 */
esp_err_t photo_client_fetch_command(device_command_t *cmd_out)
{
    memset(cmd_out, 0, sizeof(*cmd_out));

    char url[512];
    snprintf(url, sizeof(url), "%s/api/device/command", g_server_url);

    uint8_t *json_buf = NULL;
    size_t json_len = 0;
    esp_err_t ret = http_get_to_buf(url, &json_buf, &json_len, false, 0);
    if (ret != ESP_OK) return ret;

    cJSON *root = cJSON_ParseWithLength((char *)json_buf, (int)json_len);
    free(json_buf);
    if (!root) return ESP_FAIL;

    cJSON *cmd_obj = cJSON_GetObjectItem(root, "command");
    if (cmd_obj && cJSON_IsObject(cmd_obj)) {
        cmd_out->has_command = true;
        cJSON *cmd_str = cJSON_GetObjectItem(cmd_obj, "cmd");
        if (cmd_str && cmd_str->valuestring) {
            strncpy(cmd_out->cmd, cmd_str->valuestring, sizeof(cmd_out->cmd) - 1);
        }
        cJSON *target_id = cJSON_GetObjectItem(cmd_obj, "target_id");
        if (target_id && target_id->valuestring) {
            strncpy(cmd_out->target_id, target_id->valuestring, sizeof(cmd_out->target_id) - 1);
        }
        cJSON *channel = cJSON_GetObjectItem(cmd_obj, "channel");
        if (channel) cmd_out->channel = channel->valueint;
        cJSON *state = cJSON_GetObjectItem(cmd_obj, "state");
        if (state) cmd_out->state = state->valueint;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 安全释放下载返回的动态内存
 *
 * @param buf 指向需要释放内存缓冲区的指针
 */
void photo_client_free_buf(uint8_t *buf)
{
    if (buf) heap_caps_free(buf);
}
