#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "http.h"
#include "test.h"
#include <string.h>

static HttpParseResult parse(const char *text, HttpRequest *req)
{
    static char buf[8192];
    size_t n = strlen(text);
    memcpy(buf, text, n + 1u);
    memset(req, 0, sizeof(*req));
    return http_parse_request(buf, n, req);
}

int main(void)
{
    HttpRequest r;

    /* a complete GET */
    CHECK(parse("GET /notes HTTP/1.1\r\nHost: x\r\n\r\n", &r) == HTTP_PARSE_OK);
    CHECK(strcmp(r.method, "GET") == 0);
    CHECK(strcmp(r.path, "/notes") == 0);
    CHECK(r.minor_version == 1);

    /* header lookup is case-insensitive */
    size_t hl = 0u;
    const char *h = http_find_header(&r, "host", &hl);
    CHECK(h != NULL && hl == 1u && h[0] == 'x');
    CHECK(http_find_header(&r, "absent", &hl) == NULL);

    /* a body with content-length */
    CHECK(parse("POST /n HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", &r) == HTTP_PARSE_OK);
    CHECK(r.content_length == 5);
    CHECK(r.body_len == 5u);
    CHECK(memcmp(r.body, "hello", 5u) == 0);

    /* incomplete input asks for more, it does not fail */
    CHECK(parse("GET /notes HTTP/1.1\r\nHost: x\r\n", &r) == HTTP_PARSE_NEED_MORE);
    CHECK(parse("POST /n HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nab", &r) == HTTP_PARSE_NEED_MORE);

    /* garbage and smuggling attempts are rejected */
    CHECK(parse("\x01\x02 not http\r\n\r\n", &r) == HTTP_PARSE_ERROR);
    CHECK(parse("POST /n HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 9\r\n\r\nhello", &r) == HTTP_PARSE_ERROR);
    CHECK(parse("POST /n HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", &r) == HTTP_PARSE_ERROR);

    /* HTTP/1.1 without Host is invalid */
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &r) == HTTP_PARSE_ERROR);

    TEST_REPORT("http");
}
