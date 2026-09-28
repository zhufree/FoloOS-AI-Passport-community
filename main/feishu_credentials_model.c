#include "feishu_credentials_model.h"

#include <string.h>

_Static_assert(sizeof(feishu_credentials_record_t) == 200, "preserve legacy NVS layout");

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

bool feishu_credentials_record_valid(const feishu_credentials_record_t *record, size_t size)
{
    return record != NULL && size == sizeof(*record) &&
           (record->version == 1 || record->version == 2) &&
           memchr(record->app_id, 0, sizeof(record->app_id)) != NULL &&
           memchr(record->secret, 0, sizeof(record->secret)) != NULL &&
           feishu_credentials_valid(record->app_id, record->secret);
}

bool feishu_credentials_prepare(const feishu_credentials_record_t *current,
                                 const char *app_id, const char *secret, bool clear,
                                 feishu_credentials_record_t *next)
{
    if (next == NULL) return false;
    if (clear) {
        memset(next, 0, sizeof(*next));
        next->version = 2;
        return true;
    }
    if (app_id == NULL || secret == NULL) return false;
    if (!app_id[0] && !secret[0]) {
        if (!feishu_credentials_record_valid(current, sizeof(*current))) return false;
        *next = *current;
    } else {
        if (!feishu_credentials_valid(app_id, secret)) return false;
        memset(next, 0, sizeof(*next));
        memcpy(next->app_id, app_id, strlen(app_id) + 1);
        memcpy(next->secret, secret, strlen(secret) + 1);
    }
    next->version = 2;
    next->reserved = 0;
    return true;
}
