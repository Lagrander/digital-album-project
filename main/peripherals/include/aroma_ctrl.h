#ifndef AROMA_CTRL_H
#define AROMA_CTRL_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AROMA_CH_1 = 0,
    AROMA_CH_2,
    AROMA_CH_3,
    AROMA_CH_MAX
} aroma_channel_t;

esp_err_t aroma_init(void);
esp_err_t aroma_set(aroma_channel_t channel, bool on);
esp_err_t aroma_set_all(bool on);
esp_err_t aroma_toggle(aroma_channel_t channel);
bool aroma_get_state(aroma_channel_t channel);

#ifdef __cplusplus
}
#endif

#endif
