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

