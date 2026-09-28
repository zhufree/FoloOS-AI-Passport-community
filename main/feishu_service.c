#include "feishu_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_audio.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

#define TOKEN_URL "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal"
#define ASR_URL "https://open.feishu.cn/open-apis/speech_to_text/v1/speech/stream_recognize"
#define PCM_SLOTS 4
#define HTTP_RESPONSE_MAX 4096
#define FRAME_DONE (-1)
#define FRAME_ERROR (-2)

typedef struct {
    uint32_t version;
    uint32_t enabled;
    char app_id[FEISHU_APP_ID_MAX];
    char secret[FEISHU_APP_SECRET_MAX];
} feishu_settings_t;

static const char *TAG = "feishu_asr";
static SemaphoreHandle_t s_lock;
static feishu_settings_t s_settings;
static atomic_bool s_enabled;
static atomic_bool s_configured;
static char s_token[FEISHU_TOKEN_MAX];

typedef struct {
    char text[HTTP_RESPONSE_MAX];
    size_t length;
    bool overflow;
} http_response_t;

typedef struct {
    int16_t pcm[PCM_SLOTS][FEISHU_ASR_CHUNK_SAMPLES];
    QueueHandle_t free_slots;
    QueueHandle_t ready_slots;
    SemaphoreHandle_t exited;
    const feishu_asr_control_t *control;
    atomic_bool abort;
} audio_stream_t;

esp_err_t feishu_service_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("feishu", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    size_t size = sizeof(s_settings);
    result = nvs_get_blob(handle, "settings", &s_settings, &size);
    nvs_close(handle);
    if (result == ESP_OK && size == sizeof(s_settings) && s_settings.version == 1 &&
        memchr(s_settings.app_id, 0, sizeof(s_settings.app_id)) != NULL &&
        memchr(s_settings.secret, 0, sizeof(s_settings.secret)) != NULL &&
        feishu_credentials_valid(s_settings.app_id, s_settings.secret)) {
        atomic_store(&s_configured, true);
        atomic_store(&s_enabled, s_settings.enabled == 1);
    } else {
        mbedtls_platform_zeroize(&s_settings, sizeof(s_settings));
    }
    return ESP_OK;
}

bool feishu_service_enabled(void) { return atomic_load(&s_enabled); }
bool feishu_service_configured(void) { return atomic_load(&s_configured); }

esp_err_t feishu_service_configure(bool enabled, const char *app_id,
                                    const char *secret, bool clear)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    feishu_settings_t next = s_settings;
    if (clear) memset(&next, 0, sizeof(next));
    else {
        if (app_id == NULL || secret == NULL) goto done;
        if (app_id[0] || secret[0]) {
            if (!feishu_credentials_valid(app_id, secret)) goto done;
            strlcpy(next.app_id, app_id, sizeof(next.app_id));
            strlcpy(next.secret, secret, sizeof(next.secret));
        }
        if (enabled && !feishu_credentials_valid(next.app_id, next.secret)) goto done;
        next.enabled = enabled;
        next.version = 1;
    }
    nvs_handle_t handle;
    result = nvs_open("feishu", NVS_READWRITE, &handle);
    if (result != ESP_OK) goto done;
    result = nvs_set_blob(handle, "settings", &next, sizeof(next));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) {
        mbedtls_platform_zeroize(&s_settings, sizeof(s_settings));
        s_settings = next;
        atomic_store(&s_configured, feishu_credentials_valid(next.app_id, next.secret));
        atomic_store(&s_enabled, next.enabled == 1 && atomic_load(&s_configured));
        mbedtls_platform_zeroize(s_token, sizeof(s_token));
    }
done:
    mbedtls_platform_zeroize(&next, sizeof(next));
    xSemaphoreGive(s_lock);
    return result;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if ((size_t)event->data_len >= sizeof(response->text) - response->length) {
            response->overflow = true;
            return ESP_FAIL;
        }
        memcpy(response->text + response->length, event->data, event->data_len);
        response->length += event->data_len;
        response->text[response->length] = '\0';
    }
    return ESP_OK;
}

static esp_http_client_handle_t http_create(const char *url, http_response_t *response)
{
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .keep_alive_enable = true,
        .buffer_size = 1024, .buffer_size_tx = 1024,
        .event_handler = http_event, .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    return client;
}

static esp_err_t http_post(esp_http_client_handle_t client, const char *body,
                           http_response_t *response)
{
    memset(response, 0, sizeof(*response));
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t result = esp_http_client_perform(client);
    esp_http_client_set_post_field(client, NULL, 0);
    if (response->overflow) return ESP_ERR_INVALID_SIZE;
    if (result != ESP_OK) return ESP_ERR_TIMEOUT;
    int status = esp_http_client_get_status_code(client);
    if (status == 401 || status == 403) {
        return ESP_ERR_INVALID_STATE;
    }
    return status == 200 ? ESP_OK : ESP_FAIL;
}

static esp_err_t ensure_token(esp_http_client_handle_t client, http_response_t *response)
{
    cJSON *request = cJSON_CreateObject();
    if (request == NULL) return ESP_ERR_NO_MEM;
    bool valid = cJSON_AddStringToObject(request, "app_id", s_settings.app_id) != NULL &&
                 cJSON_AddStringToObject(request, "app_secret", s_settings.secret) != NULL;
    char *body = valid ? cJSON_PrintUnformatted(request) : NULL;
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(request, "app_secret");
    if (cJSON_IsString(secret)) mbedtls_platform_zeroize(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(request);
    if (body == NULL) return ESP_ERR_NO_MEM;
    esp_err_t result = client ? http_post(client, body, response) : ESP_ERR_NO_MEM;
    mbedtls_platform_zeroize(body, strlen(body));
    cJSON_free(body);
    uint32_t expires = 0;
    if (result == ESP_OK) {
        if (!feishu_parse_token(response->text, s_token, sizeof(s_token), &expires)) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    mbedtls_platform_zeroize(response, sizeof(*response));
    return result;
}

static bool cancelled(const audio_stream_t *stream)
{
    return atomic_load(&stream->abort) || atomic_load(stream->control->cancel);
}

static void audio_reader(void *argument)
{
    audio_stream_t *stream = argument;
    int frame = FRAME_DONE;
    unsigned count = 0;
    while (!cancelled(stream) && count < FEISHU_ASR_MAX_CHUNKS) {
        /* At least two packets: first (action=1), last (action=2). */
        if (count >= 2 && atomic_load(stream->control->stop)) break;
        int slot;
        if (xQueueReceive(stream->free_slots, &slot, pdMS_TO_TICKS(40)) != pdTRUE) {
            frame = FRAME_ERROR;
            break; /* Never silently drop captured audio under network backpressure. */
        }
        if (bsp_audio_read(stream->pcm[slot], sizeof(stream->pcm[slot])) != ESP_OK) {
            xQueueSend(stream->free_slots, &slot, 0);
            frame = FRAME_ERROR;
            break;
        }
        count++;
        if (stream->control->level != NULL && !cancelled(stream)) {
            int peak = 0;
            for (unsigned i = 0; i < FEISHU_ASR_CHUNK_SAMPLES; ++i) {
                int sample = stream->pcm[slot][i];
                if (sample < 0) sample = -sample;
                if (sample > peak) peak = sample;
            }
            stream->control->level((uint8_t)(peak >= 8000 ? 100 : peak / 80),
                                    stream->control->context);
        }
        xQueueSend(stream->ready_slots, &slot, portMAX_DELAY);
    }
    xQueueSend(stream->ready_slots, &frame, portMAX_DELAY);
    xSemaphoreGive(stream->exited);
    vTaskDelete(NULL);
}

static esp_err_t send_packet(esp_http_client_handle_t client, http_response_t *response,
                              const char *stream_id, unsigned sequence, int action,
                              const int16_t *pcm, char *text, size_t capacity)
{
    size_t pcm_bytes = pcm == NULL ? 0 : FEISHU_ASR_CHUNK_SAMPLES * sizeof(*pcm);
    size_t body_capacity = 4 * ((pcm_bytes + 2) / 3) + 256;
    char *body = malloc(body_capacity);
    if (!body) return ESP_ERR_NO_MEM;
    /* stream_id is generated locally as 16 hex digits; Base64 cannot contain
     * JSON quotes. Encode directly into the request to avoid a second PCM/Base64 copy. */
    int prefix = snprintf(body, body_capacity,
        "{\"config\":{\"stream_id\":\"%s\",\"sequence_id\":%u,\"action\":%d,"
        "\"format\":\"pcm\",\"engine_type\":\"16k_auto\"},\"speech\":{\"speech\":\"",
        stream_id, sequence, action);
    if (prefix < 0 || (size_t)prefix + 4 >= body_capacity) { free(body); return ESP_ERR_INVALID_SIZE; }
    size_t encoded_size = 0;
    int code = mbedtls_base64_encode((unsigned char *)body + prefix,
                                    body_capacity - (size_t)prefix - 3, &encoded_size,
                                    (const unsigned char *)pcm, pcm_bytes);
    if (code != 0) { free(body); return ESP_ERR_INVALID_SIZE; }
    memcpy(body + prefix + encoded_size, "\"}}", 4);
    esp_err_t result = http_post(client, body, response);
    free(body);
    if (result == ESP_OK && action != 3 &&
        !feishu_parse_transcript(response->text, stream_id, sequence, text, capacity)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    return result;
}

esp_err_t feishu_service_recognize(const feishu_asr_control_t *control,
                                   char *text, size_t capacity)
{
    if (!control || !control->stop || !control->cancel || !text || capacity == 0) return ESP_ERR_INVALID_ARG;
    text[0] = '\0';
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    audio_stream_t *stream = NULL;
    http_response_t *response = NULL;
    esp_http_client_handle_t client = NULL;
    bool reader_started = false;
    unsigned sequence = 0;
    int pending = -1;
    char stream_id[17];
    snprintf(stream_id, sizeof(stream_id), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    if (!feishu_service_configured()) goto done;
    response = calloc(1, sizeof(*response));
    if (!response) { result = ESP_ERR_NO_MEM; goto done; }
    client = http_create(TOKEN_URL, response);
    if (!client) { result = ESP_ERR_NO_MEM; goto done; }
    /* Authenticate before capture, then reuse the established same-host TLS connection. */
    ESP_LOGI(TAG, "before auth: heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    result = ensure_token(client, response);
    if (result != ESP_OK || atomic_load(control->cancel)) goto done;
    if (atomic_load(control->stop)) { result = ESP_ERR_INVALID_SIZE; goto done; }
    result = esp_http_client_set_url(client, ASR_URL);
    if (result != ESP_OK) goto done;
    char authorization[FEISHU_TOKEN_MAX + 8];
    snprintf(authorization, sizeof(authorization), "Bearer %s", s_token);
    result = esp_http_client_set_header(client, "Authorization", authorization);
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    if (result != ESP_OK) goto done;
    stream = calloc(1, sizeof(*stream));
    if (!stream) { result = ESP_ERR_NO_MEM; goto done; }
    stream->control = control;
    atomic_init(&stream->abort, false);
    stream->free_slots = xQueueCreate(PCM_SLOTS, sizeof(int));
    stream->ready_slots = xQueueCreate(PCM_SLOTS + 1, sizeof(int));
    stream->exited = xSemaphoreCreateBinary();
    if (!stream->free_slots || !stream->ready_slots || !stream->exited) {
        result = ESP_ERR_NO_MEM; goto done;
    }
    for (int i = 0; i < PCM_SLOTS; i++) xQueueSend(stream->free_slots, &i, 0);
    result = bsp_audio_set_format(FEISHU_ASR_SAMPLE_RATE, 16, 1);
    if (result != ESP_OK) goto done;
    if (xTaskCreate(audio_reader, "feishu_pcm", 3072, stream, 6, NULL) != pdPASS) {
        result = ESP_ERR_NO_MEM; goto done;
    }
    reader_started = true;
    ESP_LOGI(TAG, "capture ready: heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    for (;;) {
        int slot;
        if (cancelled(stream)) { result = ESP_ERR_INVALID_STATE; break; }
        if (xQueueReceive(stream->ready_slots, &slot, pdMS_TO_TICKS(1500)) != pdTRUE) {
            result = ESP_ERR_TIMEOUT; break;
        }
        if (slot == FRAME_ERROR) { result = ESP_ERR_TIMEOUT; break; }
        if (slot == FRAME_DONE) {
            if (control->stopped && !atomic_load(control->cancel)) control->stopped(control->context);
            result = pending < 0 ? ESP_ERR_INVALID_SIZE :
                     send_packet(client, response, stream_id, sequence++, 2,
                                 stream->pcm[pending], text, capacity);
            break;
        }
        if (pending >= 0) {
            result = send_packet(client, response, stream_id, sequence,
                                 sequence == 0 ? 1 : 0, stream->pcm[pending], text, capacity);
            sequence++;
            xQueueSend(stream->free_slots, &pending, 0);
            pending = -1;
            if (result != ESP_OK) break;
        }
        pending = slot;
    }
done:
    if (stream && reader_started) {
        atomic_store(&stream->abort, true);
        /* Ready queue has room for every slot plus sentinel; reader can always exit. */
        xSemaphoreTake(stream->exited, portMAX_DELAY);
    }
    if (client && sequence && (result != ESP_OK || atomic_load(control->cancel))) {
        esp_http_client_set_timeout_ms(client, 2000);
        (void)send_packet(client, response, stream_id, sequence, 3, NULL, text, capacity);
    }
    if (client) esp_http_client_cleanup(client);
    if (stream) {
        if (stream->free_slots) vQueueDelete(stream->free_slots);
        if (stream->ready_slots) vQueueDelete(stream->ready_slots);
        if (stream->exited) vSemaphoreDelete(stream->exited);
        mbedtls_platform_zeroize(stream, sizeof(*stream));
        free(stream);
    }
    if (response) { mbedtls_platform_zeroize(response, sizeof(*response)); free(response); }
    if (atomic_load(control->cancel)) result = ESP_ERR_INVALID_STATE;
    if (result == ESP_OK && text[0] == '\0') result = ESP_ERR_NOT_FOUND;
    if (result != ESP_OK) text[0] = '\0';
    mbedtls_platform_zeroize(s_token, sizeof(s_token));
    ESP_LOGI(TAG, "released: heap=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xSemaphoreGive(s_lock);
    return result;
}

const char *feishu_service_error(esp_err_t error)
{
    switch (error) {
    case ESP_ERR_NO_MEM: return "内存不足，请退出其他操作后重试";
    case ESP_ERR_TIMEOUT: return "飞书连接超时或网络过慢，请重试";
    case ESP_ERR_INVALID_STATE: return "请检查飞书应用凭据及语音识别权限";
    case ESP_ERR_INVALID_RESPONSE: return "飞书识别失败，请检查权限及版本是否支持";
    case ESP_ERR_INVALID_SIZE: return "录音或识别结果长度不合适，请重试";
    case ESP_ERR_NOT_FOUND: return "未识别到文字，请重试";
    default: return "飞书语音识别失败，请检查网络及应用配置";
    }
}
