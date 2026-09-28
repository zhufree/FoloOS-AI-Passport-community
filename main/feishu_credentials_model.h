#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_APP_ID_MAX 64
#define FEISHU_APP_SECRET_MAX 128

/* Keep the original NVS blob layout so existing keys survive the ASR removal.
 * In version 1, reserved was an ASR-enabled flag. It is now ignored. */
typedef struct {
    uint32_t version;
    uint32_t reserved;
    char app_id[FEISHU_APP_ID_MAX];
    char secret[FEISHU_APP_SECRET_MAX];
} feishu_credentials_record_t;

bool feishu_credentials_valid(const char *app_id, const char *secret);
bool feishu_credentials_record_valid(const feishu_credentials_record_t *record, size_t size);
bool feishu_credentials_prepare(const feishu_credentials_record_t *current,
                                 const char *app_id, const char *secret, bool clear,
                                 feishu_credentials_record_t *next);
