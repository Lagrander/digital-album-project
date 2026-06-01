#ifndef APP_EVENT_H
#define APP_EVENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVENT_NONE = 0,

    APP_EVENT_PLAY_STARTUP,
    APP_EVENT_PLAY_CLICK,
    APP_EVENT_PLAY_AROMA_ON,
    APP_EVENT_PLAY_AROMA_OFF,
    APP_EVENT_PLAY_ERROR,
    APP_EVENT_STOP_AUDIO,

    APP_EVENT_AROMA_CH1_ON,
    APP_EVENT_AROMA_CH1_OFF,
    APP_EVENT_AROMA_CH2_ON,
    APP_EVENT_AROMA_CH2_OFF,
    APP_EVENT_AROMA_CH3_ON,
    APP_EVENT_AROMA_CH3_OFF,
    APP_EVENT_AROMA_ALL_OFF,

    APP_EVENT_PLAY_TTS_FILE,

    APP_EVENT_SCENE_RELAX,
    APP_EVENT_SCENE_SLEEP,
    APP_EVENT_SCENE_FOCUS,
} app_event_id_t;

typedef struct {
    app_event_id_t id;
    int param;
    const char *text_arg;
} app_event_t;

esp_err_t app_event_init(void);
esp_err_t app_event_send(app_event_id_t id, int param);
esp_err_t app_event_send_text(app_event_id_t id, const char *text_arg);

#ifdef __cplusplus
}
#endif

#endif
