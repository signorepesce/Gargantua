#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "json.h"
#include "test.h"
#include <string.h>

int main(void)
{
    static JsonToken toks[256];
    const char *js = "{\"name\":\"kate\",\"age\":33,\"ok\":true,\"deep\":{\"x\":7},\"list\":[1,2,3]}";
    int n = json_parse(js, strlen(js), toks, 256);
    CHECK(n > 0);
    CHECK(toks[0].type == JSON_OBJ);

    int k = json_object_get(js, toks, n, 0, "name");
    CHECK(k > 0);
    char out[64];
    json_copy_str(js, &toks[k], out, sizeof(out));
    CHECK(strcmp(out, "kate") == 0);

    long v = 0;
    k = json_object_get(js, toks, n, 0, "age");
    CHECK(k > 0 && json_token_long_checked(js, &toks[k], &v) == 0 && v == 33);

    int b = 0;
    k = json_object_get(js, toks, n, 0, "ok");
    CHECK(k > 0 && json_token_bool_checked(js, &toks[k], &b) == 0 && b == 1);

    /* nested access by path */
    k = json_path_get(js, toks, n, 0, "deep.x");
    CHECK(k > 0 && json_token_long_checked(js, &toks[k], &v) == 0 && v == 7);

    /* a missing key is not found */
    CHECK(json_object_get(js, toks, n, 0, "nope") <= 0);

    /* malformed input is rejected */
    const char *bad = "{\"a\":,}";
    CHECK(json_parse(bad, strlen(bad), toks, 256) <= 0);
    const char *unterminated = "{\"a\":\"b";
    CHECK(json_parse(unterminated, strlen(unterminated), toks, 256) <= 0);

    TEST_REPORT("json");
}
