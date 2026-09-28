#include "feishu_protocol.h"

#include <string.h>
#include "cJSON.h"

static bool identifier(const char *value, size_t maximum)
{
    if (value == NULL || value[0] == '\0' || strlen(value) >= maximum) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

bool feishu_credentials_valid(const char *app_id, const char *secret)
{
    return identifier(app_id, FEISHU_APP_ID_MAX) &&
           strncmp(app_id, "cli_", 4) == 0 && strlen(app_id) > 4 &&
           identifier(secret, FEISHU_APP_SECRET_MAX);
}

static bool success(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsObject(root) && cJSON_IsNumber(code) && code->valuedouble == 0;
}

bool feishu_parse_token(const char *json, char *token, size_t capacity,
                        uint32_t *expires_seconds)
{
    if (token == NULL || capacity == 0 || expires_seconds == NULL) return false;
    token[0] = '\0';
    *expires_seconds = 0;
    cJSON *root = cJSON_Parse(json);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "tenant_access_token");
    const cJSON *expire = cJSON_GetObjectItemCaseSensitive(root, "expire");
    bool valid = success(root) && cJSON_IsString(value) &&
                 identifier(value->valuestring, capacity) && cJSON_IsNumber(expire) &&
                 expire->valuedouble >= 120 && expire->valuedouble <= 7200;
    if (valid) {
        memcpy(token, value->valuestring, strlen(value->valuestring) + 1);
        *expires_seconds = (uint32_t)expire->valuedouble;
    }
    cJSON_Delete(root);
    return valid;
}

bool feishu_parse_transcript(const char *json, const char *stream_id,
                             unsigned sequence, char *text, size_t capacity)
{
    if (text == NULL || capacity == 0 || stream_id == NULL) return false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "stream_id");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(data, "sequence_id");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, "recognition_text");
    bool valid = success(root) && cJSON_IsString(id) &&
                 strcmp(id->valuestring, stream_id) == 0 && cJSON_IsNumber(seq) &&
                 seq->valuedouble == sequence && cJSON_IsString(value) &&
                 strlen(value->valuestring) < capacity;
    if (valid) memcpy(text, value->valuestring, strlen(value->valuestring) + 1);
    cJSON_Delete(root);
    return valid;
}
