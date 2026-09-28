#pragma once

#include <stdatomic.h>
#include "esp_err.h"
#include "feishu_protocol.h"

/* Init after NVS/Wi-Fi initialization. Defaults to the existing Mac recognizer. */
esp_err_t feishu_service_init(void);
bool feishu_service_enabled(void);
bool feishu_service_configured(void);
/* USB owner configuration only. Empty credentials retain the saved application.
 * Busy recording rejects updates. clear removes credentials and disables ASR. */
esp_err_t feishu_service_configure(bool enabled, const char *app_id,
                                    const char *secret, bool clear);

typedef struct {
    const atomic_bool *stop;
    const atomic_bool *cancel;
    void (*level)(uint8_t level, void *context);
    void (*stopped)(void *context);
    void *context;
} feishu_asr_control_t;

/* Blocking shared service; run only on an audio worker, never inside LVGL.
 * PCM is bounded/streamed; callback thread is not LVGL. Caller owns control
 * and output until return. stop finalizes; cancel suppresses the result.
 * Callers must keep other audio users out until this function has returned. */
esp_err_t feishu_service_recognize(const feishu_asr_control_t *control,
                                   char *text, size_t capacity);
const char *feishu_service_error(esp_err_t error);
