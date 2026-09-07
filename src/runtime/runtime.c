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

static size_t field_kind_size(FieldKind kind)
{
    switch (kind)
    {
    case FIELD_INT:    return sizeof(int);
    case FIELD_LONG:   return sizeof(long);
    case FIELD_DOUBLE: return sizeof(double);
    case FIELD_BOOL:   return sizeof(bool);
    case FIELD_STR:    return sizeof(str);
    default:        return 0u;
    }
}

int is_safe_identifier(const char *name)
{
    if ((name == NULL) || ((name[0] < 'A' || name[0] > 'Z') && (name[0] < 'a' || name[0] > 'z') && name[0] != '_')) { return 0; }
    for (size_t i = 1u; i < RUNTIME_MAX_TEXT; i++)
    {
        if (name[i] == '\0') { return 1; }
        char c = name[i];
        if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_') { return 0; }
    }
    return 0;
}

static int type_valid_nested(const TypeInfo *type, unsigned depth, unsigned *remaining)
{
    if ((depth >= NEST_DEPTH_MAX) || (*remaining == 0u)) { return 0; }
    (*remaining)--;
    if ((type == NULL) || (is_safe_identifier(type->name) == 0) || (type->field_count == 0u) || (type->field_count > RUNTIME_MAX_FIELDS) || (type->fields == NULL) || (type->size == 0u)) { return 0; }
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *field = &type->fields[i];
        size_t expected = field_kind_size(field->kind);
        size_t offset = (size_t)field->offset;
        if (field->kind == FIELD_OBJECT)
        {
            if (type_valid_nested(field->nested, depth + 1u, remaining) == 0) { return 0; }
            expected = (size_t)field->nested->size;
        }
        if ((is_safe_identifier(field->name) == 0) || (expected == 0u) || ((size_t)field->size != expected) || (offset > (size_t)type->size) || (expected > ((size_t)type->size - offset))) { return 0; }
    }
    return 1;
}

int type_valid(const TypeInfo *type)
{
    unsigned remaining = RUNTIME_MAX_TYPES;
    return type_valid_nested(type, 0u, &remaining);
}

void sink_init(Sink *s, char *buf, size_t cap)
{
    assert(s != NULL);
    assert(buf != NULL);
    assert(cap > 0u);

    s->buf      = buf;
    s->cap      = cap;
    s->len      = 0u;
    s->overflow = 0;
    s->buf[0]   = '\0';
}

static void sink_add_bytes(Sink *s, const char *text, size_t n)
{
    assert(s != NULL);
    assert(text != NULL);

    if (s->overflow != 0) { return; }
    if (n >= (s->cap - s->len))
    {
        s->overflow = 1;
        return;
    }

    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void sink_add(Sink *s, const char *text)
{
    assert(s != NULL);
    assert(text != NULL);

    sink_add_bytes(s, text, strlen(text));
}

void sink_put(Sink *s, char c)
{
    assert(s != NULL);
    assert(s->cap > 0u);

    if (s->overflow != 0) { return; }
    if ((s->len + 1u) >= s->cap)
    {
        s->overflow = 1;
        return;
    }

    s->buf[s->len] = c;
    s->len++;
}

#define SINK_ADD_LITERAL(s, lit) sink_add_bytes((s), (lit), sizeof(lit) - 1u)

static const char HEX_DIGIT[] = "0123456789abcdef";

void sink_add_escaped(Sink *s, const char *text)
{
    assert(s != NULL);
    assert(text != NULL);

    size_t plain = 0u;
    size_t i     = 0u;

    while ((i < RUNTIME_MAX_TEXT) && (text[i] != '\0'))
    {
        unsigned char c = (unsigned char)text[i];

        if ((c >= 0x20u) && (c != '"') && (c != '\\'))
        {
            i++;
            continue;
        }

        if (i > plain) { sink_add_bytes(s, text + plain, i - plain); }

        switch (c)
        {
        case '"':  SINK_ADD_LITERAL(s, "\\\""); break;
        case '\\': SINK_ADD_LITERAL(s, "\\\\"); break;
        case '\n': SINK_ADD_LITERAL(s, "\\n");  break;
        case '\r': SINK_ADD_LITERAL(s, "\\r");  break;
        case '\t': SINK_ADD_LITERAL(s, "\\t");  break;
        default:
            SINK_ADD_LITERAL(s, "\\u00");
            sink_put(s, HEX_DIGIT[(c >> 4) & 0x0Fu]);
            sink_put(s, HEX_DIGIT[c & 0x0Fu]);
            break;
        }

        i++;
        plain = i;
    }

    if (i > plain) { sink_add_bytes(s, text + plain, i - plain); }

    if (s->overflow == 0) { s->buf[s->len] = '\0'; }
}

static void sink_add_int(Sink *s, long value)
{
    assert(s != NULL);
    assert(s->cap > 0u);

    char digits[24];
    int  n = 0;

    unsigned long magnitude;
    if (value < 0L)
    {
        sink_put(s, '-');
        magnitude = (unsigned long)(-(value + 1L)) + 1uL;
    }
    else
    {
        magnitude = (unsigned long)value;
    }

    do
    {
        digits[n] = (char)('0' + (magnitude % 10uL));
        n++;
        magnitude /= 10uL;
    } while ((magnitude > 0uL) && (n < 24));

    while (n > 0)
    {
        n--;
        sink_put(s, digits[n]);
    }

    if (s->overflow == 0) { s->buf[s->len] = '\0'; }
}

const char *read_str(const void *obj, unsigned short offset)
{
    assert(obj != NULL);

    const char *value = NULL;
    memcpy(&value, (const char *)obj + offset, sizeof(value));
    return value;
}

int read_int(const void *obj, unsigned short offset)
{
    assert(obj != NULL);

    int value = 0;
    memcpy(&value, (const char *)obj + offset, sizeof(value));
    return value;
}

bool read_bool(const void *obj, unsigned short offset)
{
    bool value = false;
    memcpy(&value, (const char *)obj + offset, sizeof(value));
    return value;
}

long read_long(const void *obj, unsigned short offset)
{
    long value = 0L;
    memcpy(&value, (const char *)obj + offset, sizeof(value));
    return value;
}

double read_double(const void *obj, unsigned short offset)
{
    double value = 0.0;
    memcpy(&value, (const char *)obj + offset, sizeof(value));
    return value;
}

const char *field_kind_name(FieldKind kind)
{
    assert(kind >= FIELD_INT);
    assert(kind < FIELD_KIND_COUNT);

    switch (kind)
    {
    case FIELD_INT:    return "int";
    case FIELD_LONG:   return "long";
    case FIELD_DOUBLE: return "double";
    case FIELD_BOOL:   return "bool";
    case FIELD_STR:    return "str";
    case FIELD_OBJECT: return "object";
    default:        return "?";
    }
}

static int json_write_scalar(const FieldInfo *f, const void *obj, Sink *sink)
{
    assert(f != NULL);
    assert(sink != NULL);

    if (f->kind == FIELD_STR)
    {
        const char *value = read_str(obj, f->offset);
        if (value == NULL)
        {
            sink_add(sink, "null");
            return 0;
        }
        sink_add(sink, "\"");
        sink_add_escaped(sink, value);
        sink_add(sink, "\"");
        return 0;
    }

    if (f->kind == FIELD_INT)
    {
        sink_add_int(sink, (long)read_int(obj, f->offset));
        return 0;
    }
    if (f->kind == FIELD_LONG)
    {
        sink_add_int(sink, read_long(obj, f->offset));
        return 0;
    }
    if (f->kind == FIELD_DOUBLE)
    {
        double value = read_double(obj, f->offset);
        if (isfinite(value) == 0) { return -1; }
        char scratch[64];
        (void)snprintf(scratch, sizeof(scratch), "%.17g", value);
        sink_add(sink, scratch);
        return 0;
    }
    if (f->kind == FIELD_BOOL)
    {
        sink_add(sink, (read_bool(obj, f->offset) != false) ? "true" : "false");
        return 0;
    }

    sink_add(sink, "null");

    return 0;
}

static int json_write_nested(const TypeInfo *type, const void *obj, Sink *sink, unsigned depth)
{
    if (depth >= NEST_DEPTH_MAX) { return -1; }

    sink_add(sink, "{");

    for (unsigned i = 0u; (i < type->field_count) && (sink->overflow == 0); i++)
    {
        const FieldInfo *f = &type->fields[i];

        if (i > 0u) { sink_add(sink, ","); }
        sink_add(sink, "\"");
        sink_add(sink, f->name);
        sink_add(sink, "\":");

        int rc = (f->kind == FIELD_OBJECT)
            ? json_write_nested(f->nested, (const char *)obj + f->offset, sink, depth + 1u)
            : json_write_scalar(f, obj, sink);

        if (rc != 0) { return -1; }
    }

    sink_add(sink, "}");

    return (sink->overflow == 0) ? 0 : -1;
}

int json_write_struct(const TypeInfo *type, const void *obj, char *out, size_t cap)
{
    if ((out == NULL) || (cap == 0u)) { return -1; }
    out[0] = '\0';
    if ((obj == NULL) || (type_valid(type) == 0)) { return -1; }
    Sink sink;
    sink_init(&sink, out, cap);
    if (json_write_nested(type, obj, &sink, 0u) != 0)
    {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

RowList row_list_make(const TypeInfo *type, const void *items, size_t count)
{
    RowList list = {0};
    if ((type_valid(type) == 0) || (count > (size_t)PAGE_MAX) || (count > (size_t)INT_MAX) || (count > (SIZE_MAX / (size_t)type->size)) || ((count > 0u) && (items == NULL)))
    {
        request_fail(500, "invalid collection");
        return list;
    }
    if (count > 0u)
    {
        size_t bytes = count * (size_t)type->size;
        list.items = request_alloc(bytes);
        if (list.items == NULL) { return list; }
        memcpy(list.items, items, bytes);
    }
    list.type = type;
    list.count = (int)count;
    list.size = (count > 0u) ? (int)count : 20;
    return list;
}

int row_list_write(RowList list, int paginated, char *out, size_t cap)
{
    if ((out == NULL) || (cap == 0u)) { return -1; }
    out[0] = '\0';
    if ((type_valid(list.type) == 0) || (list.count < 0) || (list.count > PAGE_MAX) || ((list.count > 0) && (list.items == NULL)) || ((size_t)list.count > (SIZE_MAX / (size_t)list.type->size)) || ((paginated != 0) && ((list.page < 0) || (list.size <= 0) || (list.size > PAGE_MAX) || (list.count > list.size)))) { return -1; }

    Sink sink;
    sink_init(&sink, out, cap);
    if (paginated != 0)
    {
        SINK_ADD_LITERAL(&sink, "{\"items\":");
    }
    SINK_ADD_LITERAL(&sink, "[");
    for (int i = 0; (i < list.count) && (sink.overflow == 0); i++)
    {
        if (i > 0) { SINK_ADD_LITERAL(&sink, ","); }
        const void *item = (const char *)list.items +
                           (size_t)i * (size_t)list.type->size;
        if (json_write_nested(list.type, item, &sink, 0u) != 0)
        {
            out[0] = '\0';
            return -1;
        }
    }
    SINK_ADD_LITERAL(&sink, "]");
    if (paginated != 0)
    {
        SINK_ADD_LITERAL(&sink, ",\"page\":");
        sink_add_int(&sink, (long)list.page);
        SINK_ADD_LITERAL(&sink, ",\"size\":");
        sink_add_int(&sink, (long)list.size);
        SINK_ADD_LITERAL(&sink, ",\"hasMore\":");
        sink_add(&sink, (list.has_more != 0) ? "true" : "false");
        SINK_ADD_LITERAL(&sink, "}");
    }
    if (sink.overflow != 0)
    {
        out[0] = '\0';
        return -1;
    }
    return 0;
}
