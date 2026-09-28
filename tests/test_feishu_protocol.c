#include <assert.h>
#include <string.h>
#include "feishu_protocol.h"

int main(void)
{
    assert(feishu_credentials_valid("cli_test_app", "test_secret"));
    assert(!feishu_credentials_valid("cli_", "test_secret"));
    assert(!feishu_credentials_valid("wrong", "test_secret"));
    assert(!feishu_credentials_valid("cli_test_app", "injected\r\nheader"));
    assert(!feishu_credentials_valid("cli_test_app", ""));
    char long_secret[FEISHU_APP_SECRET_MAX + 1];
    memset(long_secret, 'x', sizeof(long_secret) - 1);
    long_secret[sizeof(long_secret) - 1] = '\0';
    assert(!feishu_credentials_valid("cli_test_app", long_secret));

    char token[64];
    uint32_t expires;
    assert(feishu_parse_token("{\"code\":0,\"tenant_access_token\":\"t-test\",\"expire\":7200}",
                              token, sizeof(token), &expires));
    assert(strcmp(token, "t-test") == 0 && expires == 7200);
    assert(!feishu_parse_token("{\"tenant_access_token\":\"t-test\",\"expire\":7200}",
                               token, sizeof(token), &expires));
    assert(token[0] == 0 && expires == 0);
    assert(!feishu_parse_token("{\"code\":999,\"tenant_access_token\":\"t-test\",\"expire\":7200}",
                               token, sizeof(token), &expires));
    assert(!feishu_parse_token("{\"code\":0,\"tenant_access_token\":\"t-test\",\"expire\":0}",
                               token, sizeof(token), &expires));
    assert(!feishu_parse_token("{\"code\":0,\"tenant_access_token\":\"t-test\",\"expire\":7200}",
                               token, 3, &expires));
    const char *reply = "{\"code\":0,\"data\":{\"stream_id\":\"1234567890abcdef\","
                        "\"sequence_id\":2,\"recognition_text\":\"修改 main.c\"}}";
    char text[128];
    assert(feishu_parse_transcript(reply, "1234567890abcdef", 2, text, sizeof(text)));
    assert(strcmp(text, "修改 main.c") == 0);
    assert(!feishu_parse_transcript(reply, "another_stream00", 2, text, sizeof(text)));
    assert(!feishu_parse_transcript(reply, "1234567890abcdef", 3, text, sizeof(text)));
    assert(!feishu_parse_transcript(reply, "1234567890abcdef", 2, text, 5));
    assert(!feishu_parse_transcript("{\"code\":0,\"data\":{}}", "1234567890abcdef", 2, text, sizeof(text)));
    assert(!feishu_parse_transcript("not json", "1234567890abcdef", 2, text, sizeof(text)));
    return 0;
}
