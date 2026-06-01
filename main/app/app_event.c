#include "app_event.h"

#include "audio_player.h"
#include "aroma_ctrl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "APP_EVENT";

enum {
    app_event_queue_length = 10,
    app_event_task_stack_size = 4096,
    app_event_task_priority = 5,
};

static QueueHandle_t s_event_queue;
static StaticQueue_t s_event_queue_buffer;
static uint8_t s_event_queue_storage[app_event_queue_length * sizeof(app_event_t)];
static StaticTask_t s_event_task_tcb;
static StackType_t s_event_task_stack[app_event_task_stack_size];

static esp_err_t app_event_handle_audio(app_event_id_t id, const char *text_arg)
{
    switch (id) {
    case APP_EVENT_PLAY_STARTUP:
        return audio_player_play_effect(AUDIO_EFFECT_STARTUP);
    case APP_EVENT_PLAY_CLICK:
        return audio_player_play_effect(AUDIO_EFFECT_CLICK);
    case APP_EVENT_PLAY_AROMA_ON:
        return audio_player_play_effect(AUDIO_EFFECT_AROMA_ON);
    case APP_EVENT_PLAY_AROMA_OFF:
        return audio_player_play_effect(AUDIO_EFFECT_AROMA_OFF);
    case APP_EVENT_PLAY_ERROR:
        return audio_player_play_effect(AUDIO_EFFECT_ERROR);
    case APP_EVENT_STOP_AUDIO:
        return audio_player_stop();
    case APP_EVENT_PLAY_TTS_FILE:
        if (text_arg == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        return audio_player_play_tts_file(text_arg);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t app_event_handle_aroma(app_event_id_t id)
{
    switch (id) {
    case APP_EVENT_AROMA_CH1_ON:
        return aroma_set(AROMA_CH_1, true);
    case APP_EVENT_AROMA_CH1_OFF:
        return aroma_set(AROMA_CH_1, false);
    case APP_EVENT_AROMA_CH2_ON:
        return aroma_set(AROMA_CH_2, true);
    case APP_EVENT_AROMA_CH2_OFF:
        return aroma_set(AROMA_CH_2, false);
    case APP_EVENT_AROMA_CH3_ON:
        return aroma_set(AROMA_CH_3, true);
    case APP_EVENT_AROMA_CH3_OFF:
        return aroma_set(AROMA_CH_3, false);
    case APP_EVENT_AROMA_ALL_OFF:
        return aroma_set_all(false);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t app_event_handle_scene(app_event_id_t id)
{
    esp_err_t ret = ESP_OK;

    switch (id) {
    case APP_EVENT_SCENE_RELAX:
        ret = audio_player_play_effect(AUDIO_EFFECT_AROMA_ON);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_1, true);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_2, false);
        if (ret != ESP_OK) {
            return ret;
        }
        return aroma_set(AROMA_CH_3, false);
    case APP_EVENT_SCENE_SLEEP:
        ret = audio_player_play_effect(AUDIO_EFFECT_AROMA_ON);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_2, true);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_1, false);
        if (ret != ESP_OK) {
            return ret;
        }
        return aroma_set(AROMA_CH_3, false);
    case APP_EVENT_SCENE_FOCUS:
        ret = audio_player_play_effect(AUDIO_EFFECT_CLICK);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_3, true);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = aroma_set(AROMA_CH_1, false);
        if (ret != ESP_OK) {
            return ret;
        }
        return aroma_set(AROMA_CH_2, false);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t app_event_dispatch(const app_event_t *event)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    if (event == NULL || event->id == APP_EVENT_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = app_event_handle_audio(event->id, event->text_arg);
    if (ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }

    ret = app_event_handle_aroma(event->id);
    if (ret != ESP_ERR_NOT_SUPPORTED) {
        return ret;
    }

    return app_event_handle_scene(event->id);
}

static void app_event_task(void *arg)
{
    (void)arg;

    app_event_t event;
    while (1) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Dispatch event id=%d param=%d", event.id, event.param);
            esp_err_t ret = app_event_dispatch(&event);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Event id=%d failed: %s", event.id, esp_err_to_name(ret));
            }
        }
    }
}

esp_err_t app_event_init(void)
{
    if (s_event_queue != NULL) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreateStatic(
        app_event_queue_length,
        sizeof(app_event_t),
        s_event_queue_storage,
        &s_event_queue_buffer
    );
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queue");
        return ESP_FAIL;
    }

    TaskHandle_t task = xTaskCreateStatic(
        app_event_task,
        "app_event",
        app_event_task_stack_size,
        NULL,
        app_event_task_priority,
        s_event_task_stack,
        &s_event_task_tcb
    );
    if (task == NULL) {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t app_event_send(app_event_id_t id, int param)
{
    app_event_t event = {
        .id = id,
        .param = param,
        .text_arg = NULL,
    };

    esp_err_t ret = app_event_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Queue full, drop event id=%d", id);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t app_event_send_text(app_event_id_t id, const char *text_arg)
{
    if (text_arg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_event_t event = {
        .id = id,
        .param = 0,
        .text_arg = text_arg,
    };

    esp_err_t ret = app_event_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Queue full, drop text event id=%d", id);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
