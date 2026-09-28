#include "feishu_credentials.h"

#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

static SemaphoreHandle_t s_lock;
static feishu_credentials_record_t s_credentials;
static atomic_bool s_configured;

esp_err_t feishu_credentials_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("feishu", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    size_t size = sizeof(s_credentials);
    result = nvs_get_blob(handle, "settings", &s_credentials, &size);
    nvs_close(handle);
    if (result == ESP_OK && feishu_credentials_record_valid(&s_credentials, size)) {
        /* Ignore the old voice-engine selection without rewriting or deleting keys. */
        s_credentials.reserved = 0;
        atomic_store(&s_configured, true);
    } else {
        mbedtls_platform_zeroize(&s_credentials, sizeof(s_credentials));
    }
    return ESP_OK;
}

bool feishu_credentials_configured(void) { return atomic_load(&s_configured); }

esp_err_t feishu_credentials_save(const char *app_id, const char *secret, bool clear)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    feishu_credentials_record_t next = {0};
    if (!feishu_credentials_prepare(&s_credentials, app_id, secret, clear, &next)) goto done;
    nvs_handle_t handle;
    result = nvs_open("feishu", NVS_READWRITE, &handle);
    if (result != ESP_OK) goto done;
    result = nvs_set_blob(handle, "settings", &next, sizeof(next));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) {
        mbedtls_platform_zeroize(&s_credentials, sizeof(s_credentials));
        s_credentials = next;
        atomic_store(&s_configured, feishu_credentials_valid(next.app_id, next.secret));
    }
done:
    mbedtls_platform_zeroize(&next, sizeof(next));
    xSemaphoreGive(s_lock);
    return result;
}
