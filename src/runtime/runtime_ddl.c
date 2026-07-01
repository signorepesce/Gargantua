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

static int ddl_field_valid(const TypeInfo *type, unsigned index, unsigned *keys)
{
    assert(type != NULL);
    assert(keys != NULL);

    const FieldInfo *field = &type->fields[index];

    if ((field->kind == FIELD_OBJECT) || (field->nested != NULL) || ((field->flags & ~(unsigned)(FIELD_PRIMARY_KEY | FIELD_NOT_NULL | FIELD_UNIQUE)) != 0u)) { return 0; }

    if ((field->flags & (unsigned)FIELD_PRIMARY_KEY) != 0u)
    {
        if ((field->kind != FIELD_INT) || (++(*keys) > 1u)) { return 0; }
    }

    if ((field->references != NULL) || (field->reference_key != NULL))
    {
        if ((field->kind != FIELD_INT) || (is_safe_identifier(field->references) == 0) || (is_safe_identifier(field->reference_key) == 0)) { return 0; }
    }

    for (unsigned j = 0u; j < index; j++)
    {
        const FieldInfo *other = &type->fields[j];
        if ((strcmp(field->name, other->name) == 0) || ((field->offset < (size_t)other->offset + other->size) && (other->offset < (size_t)field->offset + field->size))) { return 0; }
    }

    return 1;
}

static int ddl_type_valid(const TypeInfo *type)
{
    assert(type != NULL);

    unsigned keys = 0u;
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        if (ddl_field_valid(type, i, &keys) == 0) { return 0; }
    }

    return 1;
}

static const char *sql_column_type(FieldKind kind)
{
    switch (kind)
    {
        case FIELD_STR:    return " TEXT";
        case FIELD_LONG:   return " BIGINT";
        case FIELD_DOUBLE: return " DOUBLE PRECISION";
        case FIELD_BOOL:   return " BOOLEAN";
        default:           return " INTEGER";
    }
}

static int ddl_write_column(Sink *sink, const FieldInfo *f)
{
    assert(sink != NULL);
    assert(f != NULL);

    sink_add(sink, "\"");
    sink_add(sink, f->name);
    sink_add(sink, "\"");
    sink_add(sink, sql_column_type(f->kind));

    if ((f->flags & (unsigned)FIELD_PRIMARY_KEY) != 0u)
    {
        if (f->kind != FIELD_INT) { return -1; }
        sink_add(sink, " PRIMARY KEY");
    }
    if ((f->flags & (unsigned)FIELD_NOT_NULL) != 0u) { sink_add(sink, " NOT NULL"); }
    if ((f->flags & (unsigned)FIELD_UNIQUE) != 0u) { sink_add(sink, " UNIQUE"); }
    if (f->references != NULL)
    {
        sink_add(sink, " REFERENCES \"");
        sink_add(sink, f->references);
        sink_add(sink, "\"(\"");
        sink_add(sink, f->reference_key);
        sink_add(sink, "\")");
    }

    return 0;
}

int create_table_sql_write(const TypeInfo *type, char *out, size_t cap)
{
    if ((out == NULL) || (cap == 0u)) { return -1; }

    out[0] = '\0';
    if ((type_valid(type) == 0) || (ddl_type_valid(type) == 0)) { return -1; }

    Sink sink;
    sink_init(&sink, out, cap);
    sink_add(&sink, "CREATE TABLE IF NOT EXISTS \"");
    sink_add(&sink, type->name);
    sink_add(&sink, "\" (");

    for (unsigned i = 0u; (i < type->field_count) && (i < RUNTIME_MAX_FIELDS); i++)
    {
        if (i > 0u) { sink_add(&sink, ", "); }
        if (ddl_write_column(&sink, &type->fields[i]) != 0)
        {
            out[0] = '\0';
            return -1;
        }
    }

    sink_add(&sink, ");");

    if (sink.overflow != 0)
    {
        out[0] = '\0';
        return -1;
    }

    return 0;
}

int db_create_table(const TypeInfo *type)
{
    static _Thread_local char sql[DB_MAX_SQL];
    if ((type == NULL) || (create_table_sql_write(type, sql, sizeof(sql)) != 0)) { return -1; }
    return db_execute(sql, SQL_NOARGS) < 0 ? -1 : 0;
}

static int email_valid(str text)
{
    size_t len = strlen(text);
    if (len > 254u) { return 0; }
    const char *at = strchr(text, '@');
    if (at == NULL || at == text || (size_t)(at - text) > 64u || strchr(at + 1, '@')) { return 0; }
    if (text[0] == '.' || at[-1] == '.') { return 0; }
    for (const char *p = text; p < at; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(".!#$%&'*+-/=?^_`{|}~", c))) { return 0; }
        if (c == '.' && p + 1 < at && p[1] == '.') { return 0; }
    }
    size_t label = 0u;
    int dot = 0;
    const char *domain = at + 1;
    for (size_t i = 0u; domain[i]; i++)
    {
        unsigned char c = (unsigned char)domain[i];
        if (c == '.')
        {
            if (!label || domain[i - 1u] == '-') { return 0; }
            label = 0u; dot = 1;
        }
        else
        {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') || (!label && c == '-') || ++label > 63u)
            { return 0; }
        }
    }
    return dot && label && domain[strlen(domain) - 1u] != '-';
}

int field_validate(const FieldInfo *f, const void *obj, const char *path)
{
    const FieldRules *r = &f->rules;
    char message[128];
    if (f->kind == FIELD_STR)
    {
        str value = read_str(obj, f->offset);
        if (value == NULL)
        {
            if (f->flags & FIELD_NOT_NULL) { field_error_add(path, "not_null", "must not be null"); return -1; }
            return 0;
        }
        if (r->flags & RULE_SIZE)
        {
            size_t n = 0u;
            for (size_t i = 0u; value[i] && i < RUNTIME_MAX_TEXT; i++)
            { if (((unsigned char)value[i] & 0xc0u) != 0x80u) { n++; } }
            if (n < r->size_min || n > r->size_max)
            {
                (void)snprintf(message, sizeof(message), "length must be between %u and %u Unicode code points", r->size_min, r->size_max);
                field_error_add(path, "size", message); return -1;
            }
        }
        if ((r->flags & RULE_EMAIL) && !email_valid(value))
        { field_error_add(path, "email", "must be a valid email address"); return -1; }
    }
    else if ((f->kind == FIELD_INT || f->kind == FIELD_LONG) && (r->flags & (RULE_MIN | RULE_MAX)))
    {
        long value = f->kind == FIELD_INT ? (long)read_int(obj, f->offset) : read_long(obj, f->offset);
        if ((r->flags & RULE_MIN) && value < r->min_int)
        { (void)snprintf(message, sizeof(message), "must be at least %ld", r->min_int); field_error_add(path, "min", message); return -1; }
        if ((r->flags & RULE_MAX) && value > r->max_int)
        { (void)snprintf(message, sizeof(message), "must not exceed %ld", r->max_int); field_error_add(path, "max", message); return -1; }
    }
    else if (r->flags & (RULE_MIN | RULE_MAX))
    {
        long double value = 0;
        if (f->kind == FIELD_INT) { value = read_int(obj, f->offset); }
        else if (f->kind == FIELD_LONG) { long n; memcpy(&n, (const char *)obj + f->offset, sizeof(n)); value = n; }
        else if (f->kind == FIELD_DOUBLE) { double n; memcpy(&n, (const char *)obj + f->offset, sizeof(n)); value = n; }
        if (!isfinite(value) || ((r->flags & RULE_MIN) && value < r->min))
        { (void)snprintf(message, sizeof(message), "must be at least %.21Lg", r->min); field_error_add(path, "min", message); return -1; }
        if ((r->flags & RULE_MAX) && value > r->max)
        { (void)snprintf(message, sizeof(message), "must not exceed %.21Lg", r->max); field_error_add(path, "max", message); return -1; }
    }
    return 0;
}

static int validate_nested(const TypeInfo *type, const void *obj, const char **bad, unsigned depth, const char *prefix)
{
    if (depth >= NEST_DEPTH_MAX) { return -1; }
    int failed = 0;
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *f = &type->fields[i];
        char path[NEST_DEPTH_MAX * 65];
        int n = snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "." : "", f->name);
        if (n < 0 || (size_t)n >= sizeof(path)) { return -1; }
        int rc = f->kind == FIELD_OBJECT
            ? validate_nested(f->nested, (const char *)obj + f->offset, bad, depth + 1u, path)
            : field_validate(f, obj, path);
        if (rc != 0) { if (f->kind != FIELD_OBJECT) { *bad = f->name; } failed = -1; }
    }
    return failed;
}

int validate_struct(const TypeInfo *type, const void *obj, const char **bad)
{
    if (obj == NULL || bad == NULL || !type_valid(type)) { return -1; }
    *bad = NULL;
    return validate_nested(type, obj, bad, 0u, "");
}

int text_write(str text, char *out, size_t cap)
{
    assert(out != NULL);
    assert(cap > 0u);

    if (text == NULL)
    {
        out[0] = '\0';
        return 0;
    }

    size_t n = strlen(text);
    if (n >= cap)
    {
        out[0] = '\0';
        return -1;
    }

    memcpy(out, text, n);
    out[n] = '\0';
    return 0;
}
