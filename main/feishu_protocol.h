#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEISHU_APP_ID_MAX 64
#define FEISHU_APP_SECRET_MAX 128
#define FEISHU_TOKEN_MAX 512
#define FEISHU_TRANSCRIPT_MAX 2048
#define FEISHU_ASR_SAMPLE_RATE 16000
#define FEISHU_ASR_CHUNK_SAMPLES 1600
#define FEISHU_ASR_MAX_CHUNKS 300

bool feishu_credentials_valid(const char *app_id, const char *secret);
/* Never truncate tokens or text. Server error messages are not exposed. */
bool feishu_parse_token(const char *json, char *token, size_t capacity,
                        uint32_t *expires_seconds);
bool feishu_parse_transcript(const char *json, const char *stream_id,
                             unsigned sequence, char *text, size_t capacity);
