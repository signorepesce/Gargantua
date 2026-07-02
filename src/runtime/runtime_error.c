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

