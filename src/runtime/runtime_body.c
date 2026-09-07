#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
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

static int read_int_field(const FieldInfo *f, void *obj, const char *json, const JsonToken *tok)
{
    assert(f != NULL);
    assert(tok != NULL);

    if (tok->type != JSON_PRIM) { return -1; }

    long value = 0L;
    if (json_token_long_checked(json, tok, &value) != 0) { return -1; }
    if ((value > 2147483647L) || (value < -2147483648L)) { return -1; }

    write_int(obj, f->offset, (int)value);

    return 0;
}

static int read_scalar(const FieldInfo *f, void *obj, char *json, const JsonToken *toks, int t)
{
    assert(f != NULL);
    assert(toks != NULL);

    if (f->kind == FIELD_STR) { return read_text_field(f, obj, json, &toks[t]); }
    if (f->kind == FIELD_INT) { return read_int_field(f, obj, json, &toks[t]); }
    if (f->kind == FIELD_LONG)
    {
        long value = 0L;
        if (json_token_long_checked(json, &toks[t], &value) != 0) { return -1; }
        write_long(obj, f->offset, value);
        return 0;
    }
    if (f->kind == FIELD_DOUBLE)
    {
        double value = 0.0;
        if (json_token_double_checked(json, &toks[t], &value) != 0) { return -1; }
        write_double(obj, f->offset, value);
        return 0;
    }
    if (f->kind == FIELD_BOOL)
    {
        int value = 0;
        if (json_token_bool_checked(json, &toks[t], &value) != 0) { return -1; }
        write_bool(obj, f->offset, value);
    }

    return 0;
}

static int json_read_nested(const TypeInfo *type, void *obj, char *json, const JsonToken *toks, int ntok, int root, unsigned depth, const char *prefix, size_t offset, int partial)
{
    if (depth >= NEST_DEPTH_MAX || toks[root].type != JSON_OBJ) { return -1; }
    int failed = 0;
    for (int k = root + 1; k < ntok; k++)
    {
        if (toks[k].parent != root || toks[k].type != JSON_STR) { continue; }
        if (string_has_no_nul(json, &toks[k]) != 0)
        { field_error_add(prefix, "unknown_field", "field names must not contain NUL"); failed = -1; continue; }
        char key[JSON_MAX_KEY_BYTES];
        json_copy_str(json, &toks[k], key, sizeof(key));
        int known = 0;
        for (unsigned j = 0u; j < type->field_count; j++)
        { if (strcmp(key, type->fields[j].name) == 0) { known = 1; break; } }
        if (!known)
        {
            char path[NEST_DEPTH_MAX * 65 + JSON_MAX_KEY_BYTES];
            (void)snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", key);
            field_error_add(path, "unknown_field", "field is not allowed"); failed = -1;
        }
        k++;
    }
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *f = &type->fields[i];
        char path[NEST_DEPTH_MAX * 65];
        int n = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", f->name);
        if (n < 0 || (size_t)n >= sizeof(path)) { return -1; }
        int t = json_object_get(json, toks, ntok, root, f->name);
        if (t < 0)
        {
            if (!partial && (f->flags & FIELD_NOT_NULL))
            { field_error_add(path, "required", "field is required"); failed = -1; }
            continue;
        }
        if (g_body_count >= JSON_MAX_TOKENS) { return -1; }
        BodyField *state = &g_body_fields[g_body_count++];
        (void)snprintf(state->path, sizeof(state->path), "%s", path);
        state->offset = offset + f->offset;
        state->field = f;
        state->is_null = toks[t].type == JSON_PRIM && toks[t].end - toks[t].start == 4 &&
                         memcmp(json + toks[t].start, "null", 4u) == 0;
        if (state->is_null && ((f->flags & FIELD_NOT_NULL) || f->kind != FIELD_STR))
        { field_error_add(path, "not_null", "field does not accept null"); failed = -1; continue; }
        int rc;
        if (f->kind == FIELD_OBJECT)
        {
            if (toks[t].type != JSON_OBJ)
            { field_error_add(path, "type", "must be an object"); failed = -1; continue; }
            rc = json_read_nested(f->nested, (char *)obj + f->offset, json, toks, ntok, t, depth + 1u, path, offset + f->offset, partial);
        }
        else
        {
            rc = read_scalar(f, obj, json, toks, t);
            if (rc != 0) { field_error_add(path, "type", "invalid value for field type"); }
            else { rc = field_validate(f, obj, path); }
        }
        if (rc != 0) { failed = -1; }
    }
    return failed;
}

int json_read_mode(const TypeInfo *type, void *obj, char *json, size_t len, int partial)
{
    g_body_count = 0u;
    g_body_type = NULL;
    g_body_partial = 0;
    if (obj == NULL || json == NULL || len == 0u || !type_valid(type))
    { request_fail_code(400, "invalid_json", "expected a JSON object"); return -1; }
    static _Thread_local JsonToken toks[JSON_MAX_TOKENS];
    int ntok = json_parse(json, len, toks, JSON_MAX_TOKENS);
    if (ntok <= 0 || toks[0].type != JSON_OBJ)
    { request_fail_code(400, "invalid_json", "expected a valid JSON object within parser limits"); return -1; }
    int rc = json_read_nested(type, obj, json, toks, ntok, 0, 0u, "", 0u, partial);
    if (rc == 0 && !partial)
    {
        const char *bad = NULL;
        rc = validate_struct(type, obj, &bad);
    }
    if (rc == 0) { g_body_type = type; g_body_partial = partial; }
    else { g_body_count = 0u; }
    return rc;
}

int json_read_struct(const TypeInfo *type, void *obj, char *json, size_t len)
{
    return json_read_mode(type, obj, json, len, 0);
}

int patch_apply(const TypeInfo *type, void *target, const void *changes)
{
    if (!g_body_partial || type != g_body_type || target == NULL || changes == NULL || request_failed()) { return -1; }
    for (unsigned i = 0u; i < g_body_count; i++)
    {
        const BodyField *state = &g_body_fields[i];
        if (state->field->kind == FIELD_OBJECT) { continue; }
        if (state->offset > type->size || state->field->size > type->size - state->offset) { return -1; }
        memcpy((char *)target + state->offset, (const char *)changes + state->offset, state->field->size);
    }
    const char *bad = NULL;
    return validate_struct(type, target, &bad);
}
