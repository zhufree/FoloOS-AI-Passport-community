#include <assert.h>
#include <string.h>
#include "feishu_credentials_model.h"

int main(void)
{
    /* Real v1 layout: the second word used to enable Feishu ASR. Preserve keys
     * whether it was enabled or disabled, but never carry that setting forward. */
    struct {
        uint32_t version;
        uint32_t enabled;
        char app_id[64];
        char secret[128];
    } legacy = {1, 1, "cli_fixture", "test_secret"};
    feishu_credentials_record_t current, next;
    assert(sizeof(legacy) == sizeof(current));
    memcpy(&current, &legacy, sizeof(current));
    assert(feishu_credentials_record_valid(&current, sizeof(current)));
    assert(feishu_credentials_prepare(&current, "", "", false, &next));
    assert(next.version == 2 && next.reserved == 0);
    assert(strcmp(next.app_id, legacy.app_id) == 0);
    assert(strcmp(next.secret, legacy.secret) == 0);
    assert(current.version == 1 && current.reserved == 1); /* no mutation on read */
    assert(feishu_credentials_record_valid(&next, sizeof(next)));

    legacy.enabled = 0;
    memcpy(&current, &legacy, sizeof(current));
    assert(feishu_credentials_prepare(&current, "", "", false, &next));
    assert(strcmp(next.secret, "test_secret") == 0);
    assert(feishu_credentials_prepare(&current, "cli_new", "new_test_secret", false, &next));
    assert(strcmp(next.secret, "new_test_secret") == 0);
    assert(!feishu_credentials_prepare(&current, "cli_new", "", false, &next));
    assert(!feishu_credentials_prepare(&current, "", "new_test_secret", false, &next));
    assert(!feishu_credentials_prepare(NULL, "", "", false, &next));
    assert(feishu_credentials_prepare(&current, "", "", true, &next));
    assert(next.version == 2 && next.reserved == 0 && next.app_id[0] == 0 && next.secret[0] == 0);
    assert(!feishu_credentials_record_valid(&next, sizeof(next)));
    assert(!feishu_credentials_record_valid(&current, sizeof(current) - 1));
    current.version = 99;
    assert(!feishu_credentials_record_valid(&current, sizeof(current)));
    current.version = 1;
    memset(current.secret, 'x', sizeof(current.secret));
    assert(!feishu_credentials_record_valid(&current, sizeof(current)));
    assert(!feishu_credentials_valid("cli_", "test_secret"));
    assert(!feishu_credentials_valid("cli_valid", "injected\nheader"));
    return 0;
}
