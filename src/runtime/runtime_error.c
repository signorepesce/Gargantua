#define _POSIX_C_SOURCE 200809L
#include "gargantua.h"
#include "json.h"
#include "http.h"
#include "arena.h"
#include "db.h"
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNTIME_MAX_FIELDS 64
#define RUNTIME_MAX_TYPES 4096u

#define RUNTIME_MAX_TEXT (1024u * 1024u)

typedef struct
{
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} Sink;
#include "runtime_internal.h"

int path_param_int(const RequestParams *p, int index, int *ok)
{
    assert(p != NULL);
    assert(ok != NULL);

    *ok = 0;
    if ((index < 0) || (index >= p->path_count) || (index >= PATH_PARAM_MAX)) { return 0; }

    const char *s = p->path_value[index];
    if (s[0] == '\0') { return 0; }

    char *end = NULL;
    errno = 0;
    long value = strtol(s, &end, 10);
    if ((errno == ERANGE) || (end == s) || (*end != '\0') || (value < (long)INT_MIN) || (value > (long)INT_MAX)) { return 0; }

    *ok = 1;
    return (int)value;
}

str path_param_text(const RequestParams *p, int index)
{
    assert(p != NULL);
    assert(index >= 0);

    if ((index >= p->path_count) || (index >= PATH_PARAM_MAX)) { return ""; }
    return p->path_value[index];
}

typedef struct
{
    char field[NEST_DEPTH_MAX * 65];
    char code[32];
    char message[128];
} FieldError;


static _Thread_local FieldError g_errors[ERROR_FIELDS_MAX];
static _Thread_local unsigned g_error_count;
static _Thread_local int g_error_truncated;
static _Thread_local char g_error_code[32];
static _Thread_local int  g_fail_status;
static _Thread_local char g_fail_msg[FAIL_MESSAGE_LEN];

void request_fail(int status, str message)
{
    assert(status >= 100);
    assert(status < 600);

    if (g_fail_status != 0) { return; }
    g_fail_status = status;

    if (message == NULL)
    {
        g_fail_msg[0] = '\0';
        return;
    }

    size_t n = strlen(message);
    if (n >= (size_t)FAIL_MESSAGE_LEN) { n = (size_t)FAIL_MESSAGE_LEN - 1u; }
    memcpy(g_fail_msg, message, n);
    g_fail_msg[n] = '\0';
}

int request_failed(void)
{
    assert(g_fail_status >= 0);
    assert(g_fail_status < 600);

    return (g_fail_status != 0) ? 1 : 0;
}

int request_fail_status(void)
{
    assert(g_fail_status >= 0);
    assert(g_fail_status < 600);

    return g_fail_status;
}

str request_fail_message(void)
{
    assert(FAIL_MESSAGE_LEN > 0);
    assert(g_fail_msg[FAIL_MESSAGE_LEN - 1] == '\0');

    return g_fail_msg;
}

void request_fail_reset(void)
{
    assert(FAIL_MESSAGE_LEN > 0);
    assert(sizeof(g_fail_msg) == (size_t)FAIL_MESSAGE_LEN);

    g_fail_status = 0;
    g_fail_msg[0] = '\0';
    body_reset();
    g_error_count = 0u;
    g_error_truncated = 0;
    g_error_code[0] = '\0';
}

void request_fail_code(int status, str code, str message)
{
    if (request_failed()) { return; }
    request_fail(status, message);
    (void)snprintf(g_error_code, sizeof(g_error_code), "%s", code);
}

void field_error_add(str field, str code, str message)
{
    if (request_failed() && strcmp(g_error_code, "validation_failed") != 0) { return; }
    request_fail_code(400, "validation_failed", "request validation failed");
    if (g_error_count >= ERROR_FIELDS_MAX) { g_error_truncated = 1; return; }
    FieldError *error = &g_errors[g_error_count++];
    (void)snprintf(error->field, sizeof(error->field), "%s", field);
    (void)snprintf(error->code, sizeof(error->code), "%s", code);
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static str error_code_for_status(int status)
{
    switch (status)
    {
        case 400: return "bad_request";
        case 401: return "unauthorized";
        case 403: return "forbidden";
        case 404: return "not_found";
        case 405: return "method_not_allowed";
        case 409: return "conflict";
        case 413: return "payload_too_large";
        case 415: return "unsupported_media_type";
        case 417: return "expectation_failed";
        case 422: return "unprocessable_content";
        case 429: return "rate_limited";
        case 503: return "unavailable";
        default: return status >= 500 ? "internal_error" : "request_failed";
    }
}

static void sink_quoted(Sink *sink, const char *text)
{
    sink_put(sink, '"');
    sink_add_escaped(sink, text);
    sink_put(sink, '"');
}

int error_json_write(int status, str message, char *out, size_t cap)
{
    if (out == NULL || cap == 0u) { return -1; }
    Sink sink;
    sink_init(&sink, out, cap);
    int active = g_fail_status == status;
    char number[32];
    (void)snprintf(number, sizeof(number), "{\"status\":%d,\"code\":", status);
    sink_add(&sink, number);
    sink_quoted(&sink, active && *g_error_code ? g_error_code : error_code_for_status(status));
    sink_add(&sink, ",\"error\":");
    sink_quoted(&sink, active ? g_fail_msg : (message ? message : "request failed"));
    sink_add(&sink, ",\"fields\":[");
    for (unsigned i = 0u; active && i < g_error_count; i++)
    {
        const FieldError *e = &g_errors[i];
        if (i) { sink_add(&sink, ","); }
        sink_add(&sink, "{\"field\":"); sink_quoted(&sink, e->field);
        sink_add(&sink, ",\"code\":"); sink_quoted(&sink, e->code);
        sink_add(&sink, ",\"message\":"); sink_quoted(&sink, e->message);
        sink_add(&sink, "}");
    }
    sink_add(&sink, active && g_error_truncated ? "],\"truncated\":true}" : "],\"truncated\":false}");
    return sink.overflow ? -1 : 0;
}

static int hex_digit_value(char c)
{
    if ((c >= '0') && (c <= '9')) { return c - '0'; }
    if ((c >= 'a') && (c <= 'f')) { return (c - 'a') + 10; }
    if ((c >= 'A') && (c <= 'F')) { return (c - 'A') + 10; }
    return -1;
}

int url_decode(char *text)
{
    assert(text != NULL);
    assert(RUNTIME_MAX_TEXT > 0u);

    size_t read  = 0u;
    size_t write = 0u;

    while ((read < RUNTIME_MAX_TEXT) && (text[read] != '\0'))
    {
        char c = text[read];

        if (c == '%')
        {
            int hi = (text[read + 1u] != '\0') ? hex_digit_value(text[read + 1u]) : -1;
            int lo = (hi >= 0) ? hex_digit_value(text[read + 2u]) : -1;
            if (lo < 0) { return -1; }
            unsigned char decoded = (unsigned char)((hi * 16) + lo);
            if ((decoded < 0x20u) || (decoded == 0x7fu)) { return -1; }
            text[write] = (char)decoded;
            read += 3u;
            write++;
        }
        else
        {
            text[write] = c;
            read++;
            write++;
        }
    }

    if (read == RUNTIME_MAX_TEXT)
    {
        text[write] = '\0';
        return -1;
    }
    text[write] = '\0';
    return 0;
}
