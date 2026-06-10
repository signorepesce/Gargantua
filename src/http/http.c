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

