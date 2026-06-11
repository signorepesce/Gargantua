#include "http.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

static int is_token_char(unsigned char c)
{
    if ((c >= (unsigned char)'0' && c <= (unsigned char)'9') || (c >= (unsigned char)'A' && c <= (unsigned char)'Z') || (c >= (unsigned char)'a' && c <= (unsigned char)'z')) { return 1; }
    return strchr("!#$%&'*+-.^_`|~", (int)c) != NULL;
}

static int expect_crlf(const char **p, const char *end)
{
    assert(p != NULL && *p != NULL && end != NULL);
    if ((size_t)(end - *p) < 2u || (*p)[0] != '\r' || (*p)[1] != '\n') { return -1; }
    *p += 2;
    return 0;
}

static int read_token(const char **p, const char *end, char *out, size_t cap)
{
    size_t n = 0u;

    assert(p != NULL && *p != NULL && end != NULL);
    assert(out != NULL && cap > 0u);

    while (*p < end && is_token_char((unsigned char)**p) != 0)
    {
        if (n + 1u >= cap) { return -1; }
        out[n++] = **p;
        (*p)++;
    }
    if (n == 0u) { return -1; }
    out[n] = '\0';
    return 0;
}

static int parse_request_line(const char **p, const char *end, HttpRequest *req)
{
    const char *start;
    size_t n = 0u;

    if (read_token(p, end, req->method, sizeof(req->method)) != 0 || *p >= end || **p != ' ') { return -1; }
    (*p)++;

    start = *p;
    while (*p < end && **p != ' ')
    {
        unsigned char c = (unsigned char)**p;
        if (c <= 0x20u || c == 0x7fu || c == (unsigned char)'#') { return -1; }
        if (++n >= sizeof(req->path)) { return -1; }
        (*p)++;
    }
    if (n == 0u || *p >= end || **p != ' ' || (start[0] != '/' && !(n == 1u && start[0] == '*'))) { return -1; }
    memcpy(req->path, start, n);
    req->path[n] = '\0';
    (*p)++;

    if ((size_t)(end - *p) < 8u || memcmp(*p, "HTTP/1.", 7u) != 0 || ((*p)[7] != '0' && (*p)[7] != '1')) { return -1; }
    req->minor_version = (*p)[7] - '0';
    *p += 8;
    return expect_crlf(p, end);
}

static int parse_content_length(const char *value, size_t len, long *out)
{
    unsigned long value_num = 0ul;

    if (len == 0u) { return -1; }
    for (size_t i = 0u; i < len; i++)
    {
        unsigned char c = (unsigned char)value[i];
        if (c < (unsigned char)'0' || c > (unsigned char)'9') { return -1; }
        unsigned digit = (unsigned)(c - (unsigned char)'0');
        if (value_num > ((unsigned long)LONG_MAX - digit) / 10ul) { return -1; }
        value_num = (value_num * 10ul) + digit;
    }
    *out = (long)value_num;
    return 0;
}

static int header_name_is(const HttpHeader *h, const char *name)
{
    size_t n = strlen(name);
    return h->name_len == n && strncasecmp(h->name, name, n) == 0;
}

typedef struct
{
    int host;
    int content_length;
    int transfer_encoding;
    int authorization;
    int content_type;
    int expect;
} HeaderCounts;

static int parse_header_name(const char **p, const char *end, size_t *name_len)
{
    assert(p != NULL);
    assert(name_len != NULL);

    *name_len = 0u;
    while ((*p < end) && (**p != ':'))
    {
        if (is_token_char((unsigned char)**p) == 0) { return -1; }
        (*name_len)++;
        (*p)++;
    }

    if ((*name_len == 0u) || (*p >= end) || (**p != ':')) { return -1; }

    (*p)++;
    while ((*p < end) && ((**p == ' ') || (**p == '\t'))) { (*p)++; }

    return 0;
}

static int parse_header_value(const char **p, const char *end,const char *value, size_t *value_len)
{
    assert(p != NULL);
    assert(value != NULL);
    assert(value_len != NULL);

    *value_len = 0u;
    while ((*p < end) && (**p != '\r') && (**p != '\n'))
    {
        unsigned char c = (unsigned char)**p;
        if (((c < 0x20u) && (c != (unsigned char)'\t')) || (c == 0x7fu)) { return -1; }
        (*value_len)++;
        (*p)++;
    }

    while ((*value_len > 0u) && ((value[*value_len - 1u] == ' ') || (value[*value_len - 1u] == '\t'))) { (*value_len)--; }

    return 0;
}

static int host_value_valid(const char *value, size_t value_len)
{
    assert(value != NULL);

    if (value_len == 0u) { return 0; }
    for (size_t i = 0u; i < value_len; i++)
    {
        if ((value[i] == ' ') || (value[i] == '\t') || (value[i] == ',')) { return 0; }
    }

    return 1;
}

static int count_known_header(const HttpHeader *h, HeaderCounts *seen, HttpRequest *req)
{
    assert(h != NULL);
    assert(seen != NULL);
    assert(req != NULL);

    if (header_name_is(h, "Host") != 0)
    {
        seen->host++;
        return ((seen->host > 1) || (host_value_valid(h->value, h->value_len) == 0))
                   ? -1 : 0;
    }
    if (header_name_is(h, "Content-Length") != 0)
    {
        seen->content_length++;
        return ((seen->content_length > 1) || (parse_content_length(h->value, h->value_len, &req->content_length) != 0))
                   ? -1 : 0;
    }
    if (header_name_is(h, "Transfer-Encoding") != 0)
    {
        seen->transfer_encoding++;
        return 0;
    }
    if (header_name_is(h, "Authorization") != 0) { return (++seen->authorization > 1) ? -1 : 0; }
    if (header_name_is(h, "Content-Type") != 0) { return (++seen->content_type > 1) ? -1 : 0; }
    if (header_name_is(h, "Expect") != 0) { return (++seen->expect > 1) ? -1 : 0; }

    return 0;
}

static int parse_headers(const char **p, const char *end, HttpRequest *req)
{
    assert(p != NULL);
    assert(req != NULL);

    HeaderCounts seen = {0};

    while (*p < end)
    {
        if (((size_t)(end - *p) >= 2u) && ((*p)[0] == '\r') && ((*p)[1] == '\n'))
        {
            *p += 2;
            if ((req->minor_version == 1) && (seen.host != 1)) { return -1; }
            return ((seen.transfer_encoding == 0) && (seen.content_length <= 1))
                       ? 0 : -1;
        }
        if (req->num_headers >= HTTP_MAX_HEADERS) { return -1; }

        const char *name     = *p;
        size_t      name_len = 0u;
        if (parse_header_name(p, end, &name_len) != 0) { return -1; }

        const char *value     = *p;
        size_t      value_len = 0u;
        if (parse_header_value(p, end, value, &value_len) != 0) { return -1; }
        if (expect_crlf(p, end) != 0) { return -1; }

        HttpHeader *h = &req->headers[req->num_headers++];
        h->name      = name;
        h->name_len  = name_len;
        h->value     = value;
        h->value_len = value_len;

        if (count_known_header(h, &seen, req) != 0) { return -1; }
    }

    return -1;
}

static int header_end(const char *buf, size_t len, size_t *end_out)
{
    if (len < 4u) { return 0; }
    for (size_t i = 0u; i <= len - 4u; i++)
    {
        if (buf[i] == '\r' && buf[i + 1u] == '\n' && buf[i + 2u] == '\r' && buf[i + 3u] == '\n')
        {
            *end_out = i + 4u;
            return 1;
        }
    }
    return 0;
}

HttpParseResult http_parse_request(char *buf, size_t len, HttpRequest *req)
{
    const char *p;
    const char *end;
    size_t headers = 0u;

    if (buf == NULL || req == NULL) { return HTTP_PARSE_ERROR; }
    memset(req, 0, sizeof(*req));
    req->content_length = -1;
    if (len == 0u) { return HTTP_PARSE_NEED_MORE; }
    if (header_end(buf, len, &headers) == 0)
    {
        return (len > HTTP_MAX_HEADER_BYTES) ? HTTP_PARSE_ERROR
                                             : HTTP_PARSE_NEED_MORE;
    }
    if (headers > HTTP_MAX_HEADER_BYTES) { return HTTP_PARSE_ERROR; }

    p = buf;
    end = buf + len;

    if (parse_request_line(&p, end, req) != 0 || parse_headers(&p, end, req) != 0) { return HTTP_PARSE_ERROR; }

    req->header_len = (size_t)(p - buf);
    req->body = p;
    if (req->content_length >= 0 && (unsigned long long)req->content_length > (unsigned long long)SIZE_MAX) { return HTTP_PARSE_ERROR; }
    req->body_len = req->content_length >= 0 ? (size_t)req->content_length : 0u;
    if (req->body_len > (size_t)(end - p)) { return HTTP_PARSE_NEED_MORE; }
    req->message_len = req->header_len + req->body_len;
    return HTTP_PARSE_OK;
}

const char *http_find_header(const HttpRequest *req, const char *name, size_t *out_len)
{
    if (req == NULL || name == NULL) { return NULL; }
    for (int i = 0; i < req->num_headers; i++)
    {
        const HttpHeader *h = &req->headers[i];
        if (header_name_is(h, name) != 0)
        {
            if (out_len != NULL) { *out_len = h->value_len; }
            return h->value;
        }
    }
    return NULL;
}
