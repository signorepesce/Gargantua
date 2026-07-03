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

static void write_str(void *obj, unsigned short offset, const char *value)
{
    assert(obj != NULL);
    assert(value != NULL);

    const char *stored = value;
    memcpy((char *)obj + offset, &stored, sizeof(stored));
}

static void write_int(void *obj, unsigned short offset, int value)
{
    assert(obj != NULL);
    memcpy((char *)obj + offset, &value, sizeof(value));
}

static void write_long(void *obj, unsigned short offset, long value)
{
    memcpy((char *)obj + offset, &value, sizeof(value));
}

static void write_double(void *obj, unsigned short offset, double value)
{
    memcpy((char *)obj + offset, &value, sizeof(value));
}

static void write_bool(void *obj, unsigned short offset, int value)
{
    bool stored = (value != 0);
    memcpy((char *)obj + offset, &stored, sizeof(stored));
}

static int string_has_no_nul(const char *json, const JsonToken *tok)
{
    assert(json != NULL);
    assert(tok != NULL);
    size_t start = (size_t)tok->start;
    size_t end = (size_t)tok->end;
    for (size_t i = start; i < end; i++)
    {
        if (json[i] != '\\') { continue; }
        i++;
        if (i >= end) { return -1; }
        if (json[i] == 'u')
        {
            if ((end - i) < 5u) { return -1; }
            if (json[i + 1u] == '0' && json[i + 2u] == '0' && json[i + 3u] == '0' && json[i + 4u] == '0') { return -1; }
            i += 4u;
        }
    }

    return 0;
}

static int unescape_in_place(char *json, const JsonToken *tok)
{
    if (string_has_no_nul(json, tok) != 0) { return -1; }
    size_t cap = (size_t)(tok->end - tok->start) + 1u;
    json_copy_str(json, tok, json + tok->start, cap);
    return 0;
}

typedef struct
{
    char path[NEST_DEPTH_MAX * 65];
    size_t offset;
    const FieldInfo *field;
    int is_null;
} BodyField;

static _Thread_local BodyField g_body_fields[JSON_MAX_TOKENS];
static _Thread_local unsigned g_body_count;
static _Thread_local const TypeInfo *g_body_type;
static _Thread_local int g_body_partial;

void body_reset(void)
{
    g_body_count = 0u; g_body_type = NULL; g_body_partial = 0;
}

int body_has_key(str path)
{
    if (path == NULL) { return 0; }
    for (unsigned i = 0u; i < g_body_count; i++)
    { if (strcmp(path, g_body_fields[i].path) == 0) { return 1; } }
    return 0;
}

int body_is_null(str path)
{
    if (path == NULL) { return 0; }
    for (unsigned i = 0u; i < g_body_count; i++)
    { if (strcmp(path, g_body_fields[i].path) == 0) { return g_body_fields[i].is_null; } }
    return 0;
}

static int read_text_field(const FieldInfo *f, void *obj, char *json, const JsonToken *tok)
{
    assert(f != NULL);
    assert(tok != NULL);

    int start = tok->start;
    int end   = tok->end;

    if ((tok->type == JSON_PRIM) && ((end - start) == 4) && (memcmp(json + start, "null", 4u) == 0) && ((f->flags & (unsigned)FIELD_NOT_NULL) == 0u))
    {
        const char *value = NULL;
        memcpy((char *)obj + f->offset, &value, sizeof(value));
        return 0;
    }

    if ((tok->type != JSON_STR) || (unescape_in_place(json, tok) != 0)) { return -1; }

    write_str(obj, f->offset, json + start);

    return 0;
}

