#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "response.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct
{
    char name[RESPONSE_NAME_CAP];
    char value[RESPONSE_VALUE_CAP];
} ResponseHeader;

static _Thread_local ResponseHeader headers[RESPONSE_MAX_HEADERS];
static _Thread_local int header_count;
static _Thread_local int status_override;

int response_token(const char *text)
{
    if ((text == NULL) || (*text == '\0')) { return 0; }
    for (size_t i = 0u; text[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL)) { return 0; }
    }
    return 1;
}

int response_value(const char *text)
{
    if (text == NULL) { return 0; }
    for (size_t i = 0u; text[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if ((c == 127u) || ((c < 32u) && (c != '\t'))) { return 0; }
    }
    return 1;
}

static int is_reserved_header(const char *name)
{
    static const char *const names[] =
    {
        "Content-Length", "Transfer-Encoding", "Connection", "Trailer",
        "TE", "Upgrade", "Keep-Alive", "Content-Type", "Allow", "Vary",
        "X-Content-Type-Options", "Referrer-Policy", "Content-Security-Policy",
        "Strict-Transport-Security", "X-Frame-Options", "X-XSS-Protection",
        "Content-Security-Policy-Report-Only", "Permissions-Policy",
        "Cross-Origin-Resource-Policy", "Cross-Origin-Opener-Policy",
        "Cross-Origin-Embedder-Policy",
        "Proxy-Authenticate", "Proxy-Authorization"
    };
    if (strncasecmp(name, "Access-Control-", 15u) == 0) { return 1; }
    for (size_t i = 0u; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strcasecmp(name, names[i]) == 0) { return 1; }
    }
    return 0;
}

int response_header(const char *name, const char *value)
{
    if ((name == NULL) || (value == NULL) || (strlen(name) >= RESPONSE_NAME_CAP) || (strlen(value) >= RESPONSE_VALUE_CAP) || !response_token(name) || !response_value(value) || is_reserved_header(name)) { return -1; }
    int index = header_count;
    for (int i = 0; i < header_count; i++)
    {
        if (strcasecmp(headers[i].name, name) == 0)
        {
            index = i;
            break;
        }
    }
    if (index == RESPONSE_MAX_HEADERS) { return -1; }
    memcpy(headers[index].name, name, strlen(name) + 1u);
    memcpy(headers[index].value, value, strlen(value) + 1u);
    if (index == header_count) { header_count++; }
    return 0;
}

int response_location(const char *value)
{
    return response_header("Location", value);
}

int response_status(int code)
{
    if ((code < 200) || (code > 599)) { return -1; }
    status_override = code;
    return 0;
}

void response_reset(void)
{
    header_count = 0;
    status_override = 0;
}

int response_status_get(int fallback)
{
    return (status_override != 0) ? status_override : fallback;
}

int response_write(char *out, size_t cap)
{
    if ((out == NULL) || (cap == 0u)) { return -1; }
    size_t at = 0u;
    out[0] = '\0';
    for (int i = 0; i < header_count; i++)
    {
        int n = snprintf(out + at, cap - at, "%s: %s\r\n", headers[i].name, headers[i].value);
        if ((n < 0) || ((size_t)n >= cap - at)) { return -1; }
        at += (size_t)n;
    }
    return (int)at;
}
