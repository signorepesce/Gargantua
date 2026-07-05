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

void request_fail_field(str field)
{
    assert(field != NULL);
    assert(FAIL_MESSAGE_LEN > 16);

    field_error_add(field, "required", "field is required");
}

static void copy_bounded(char *dst, const char *src, size_t n)
{
    assert(dst != NULL);
    assert(src != NULL);

    size_t take = (n < (size_t)(PARAM_VALUE_LEN - 1)) ? n
                                                   : (size_t)(PARAM_VALUE_LEN - 1);
    memcpy(dst, src, take);
    dst[take] = '\0';
}

int query_parse(RequestParams *p, char *query)
{
    assert(p != NULL);
    assert(QUERY_PARAM_MAX > 0);

    p->query_count = 0;
    p->invalid = 0;

    if (query == NULL) { return 0; }

    char *cursor = query;
    while (*cursor != '\0')
    {
        if (p->query_count >= QUERY_PARAM_MAX) { p->invalid = 1; return -1; }
        char *amp = strchr(cursor, '&');
        if (amp != NULL) { *amp = '\0'; }

        char *eq = strchr(cursor, '=');
        if ((cursor[0] == '\0') || (eq == NULL) || (eq == cursor))
        {
            p->invalid = 1;
            return -1;
        }
        *eq = '\0';
        for (char *s = cursor; *s != '\0'; s++)
        {
            if (*s == '+') { *s = ' '; }
        }
        for (char *s = eq + 1; *s != '\0'; s++)
        {
            if (*s == '+') { *s = ' '; }
        }
        if ((url_decode(cursor) != 0) || (url_decode(eq + 1) != 0) || (strlen(cursor) >= (size_t)PARAM_VALUE_LEN) || (strlen(eq + 1) >= (size_t)PARAM_VALUE_LEN))
        {
            p->invalid = 1;
            return -1;
        }
        for (int i = 0; i < p->query_count; i++)
        {
            if (strcmp(p->query_name[i], cursor) == 0)
            {
                p->invalid = 1;
                return -1;
            }
        }
        copy_bounded(p->query_name[p->query_count], cursor, strlen(cursor));
        copy_bounded(p->query_value[p->query_count], eq + 1, strlen(eq + 1));
        p->query_count++;

        if (amp == NULL) { break; }
        cursor = amp + 1;
    }
    return 0;
}

static int query_index(const RequestParams *p, str name)
{
    assert(p != NULL);
    assert(name != NULL);

    for (int i = 0; (i < p->query_count) && (i < QUERY_PARAM_MAX); i++)
    {
        if (strcmp(p->query_name[i], name) == 0) { return i; }
    }
    return -1;
}

str query_text(const RequestParams *p, str name)
{
    assert(p != NULL);
    assert(name != NULL);

    int i = query_index(p, name);
    return (i >= 0) ? p->query_value[i] : "";
}

int query_int(const RequestParams *p, str name, int *ok)
{
    assert(p != NULL);
    assert(ok != NULL);

    int i = query_index(p, name);
    if (i < 0)
    {
        *ok = -1;
        return 0;
    }

    RequestParams one;
    one.path_count   = 0;
    one.query_count  = 0;
    one.invalid = 0;
    copy_bounded(one.path_value[0], p->query_value[i], strlen(p->query_value[i]));
    one.path_count = 1;

    return path_param_int(&one, 0, ok);
}

int query_has(const RequestParams *p, str name)
{
    assert(p != NULL);
    assert(name != NULL);

    return (query_index(p, name) >= 0) ? 1 : 0;
}

