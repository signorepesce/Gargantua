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

