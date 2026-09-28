#pragma once

#include "esp_err.h"
#include "feishu_credentials_model.h"

/* Credential storage only: no ASR, authentication or network requests. */
esp_err_t feishu_credentials_init(void);
bool feishu_credentials_configured(void);
/* Empty ID/Secret keep a valid existing record. Only an explicit clear deletes it.
 * Call from the USB bridge task, not from LVGL/button callbacks. */
esp_err_t feishu_credentials_save(const char *app_id, const char *secret, bool clear);
