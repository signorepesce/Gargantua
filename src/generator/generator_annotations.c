#include "generator.h"
#include "parse_internal.h"
#include "store.h"
#include "scheduler.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int content_type_valid(const char *text)
{
    assert(text != NULL);

    size_t n     = strlen(text);
    int    slash = 0;

    if ((n == 0u) || (n >= GENERATOR_MAX_CONTENT_TYPE)) { return 0; }

    for (size_t i = 0u; i < n; i++)
    {
        unsigned char c = (unsigned char)text[i];
        if ((c < 0x20u) || (c > 0x7Eu) || (c == (unsigned char)'"') || (c == (unsigned char)'\\')) { return 0; }
        if (c == (unsigned char)'/') { slash++; }
    }

    return (slash == 1) ? 1 : 0;
}

static int parse_interval(const char *text, long *out_ms)
{
    assert(text != NULL);
    assert(out_ms != NULL);

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);

    if ((errno != 0) || (end == text) || (value <= 0) || (value > 86400000L)) { return -1; }

    long scale = 0L;
    if (strcmp(end, "ms") == 0)     { scale = 1L; }
    else if (strcmp(end, "s") == 0) { scale = 1000L; }
    else if (strcmp(end, "m") == 0) { scale = 60000L; }
    else if (strcmp(end, "h") == 0) { scale = 3600000L; }
    else                            { return -1; }

    if (value > (86400000L / scale)) { return -1; }

    long total = value * scale;
    if ((total < 100L) || (total > 86400000L)) { return -1; }

    *out_ms = total;

    return 0;
}

static int store_capacity_valid(const char *text, long *out)
{
    assert(text != NULL);
    assert(out != NULL);

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);

    if ((errno != 0) || (end == text) || (*end != '\0') || (value < 1L) || (value > (long)PAGE_MAX)) { return 0; }

    *out = value;

    return 1;
}

static int split_store_arguments(char *work, char **type_name, char **size_text)
{
    assert(work != NULL);
    assert(type_name != NULL);
    assert(size_text != NULL);

    char *close = strrchr(work, ')');
    if ((close == NULL) || (close[1] != '\0')) { return -1; }
    *close = '\0';

    char *comma = strchr(work, ',');
    if (comma == NULL) { return -1; }
    *comma = '\0';

    *type_name = line_trim(work);
    *size_text = line_trim(comma + 1);

    return 0;
}

int parse_store_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (strncmp(line, "$store(", 7u) != 0) { return 0; }

    char work[GENERATOR_MAX_LINE];
    (void)snprintf(work, sizeof(work), "%s", line + 7);

    char *type_name = NULL;
    char *size_text = NULL;
    if (split_store_arguments(work, &type_name, &size_text) != 0)
    {
        generator_error(ctx, st->lineno, "write $store(Type, count)");
        return 1;
    }

    if (is_safe_identifier(type_name) == 0)
    {
        generator_error(ctx, st->lineno, "invalid type '%s' in $store", type_name);
        return 1;
    }

    long capacity = 0L;
    if (store_capacity_valid(size_text, &capacity) == 0)
    {
        generator_error(ctx, st->lineno, "$store requires a count from 1 to %d" " (one page, so _store_list can hold it)", PAGE_MAX);
        return 1;
    }

    for (int i = 0; i < ctx->store_count; i++)
    {
        if (word_equals(ctx->stores[i].type, type_name) == 1)
        {
            generator_error(ctx, st->lineno, "type '%s' already has $store", type_name);
            return 1;
        }
    }
    if (ctx->store_count >= GENERATOR_MAX_STORES)
    {
        generator_error(ctx, st->lineno, "too many stores (limit %d)", GENERATOR_MAX_STORES);
        return 1;
    }

    ParsedStore *store = &ctx->stores[ctx->store_count];
    memset(store, 0, sizeof(*store));
    (void)snprintf(store->type, sizeof(store->type), "%s", type_name);
    store->capacity   = (int)capacity;
    store->line       = st->lineno;
    store->file_index = ctx->file_count - 1;

    ctx->store_count++;

    return 1;
}

int parse_produces_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (strncmp(line, "$produces(", 10u) != 0) { return 0; }

    char value[GENERATOR_MAX_URL];
    if (parse_route_url(ctx, st, line, "$produces", value) != 0) { return 1; }
    if (content_type_valid(value) == 0)
    {
        generator_error(ctx, st->lineno, "invalid content type '%s' in $produces", value);
        return 1;
    }
    if (st->pending_produces[0] != '\0')
    {
        generator_error(ctx, st->lineno, "duplicate $produces");
        return 1;
    }

    (void)snprintf(st->pending_produces, sizeof(st->pending_produces), "%s", value);

    return 1;
}

int parse_repeat_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (word_equals(line, "$on_start") == 1)
    {
        if (st->task_armed != 0)
        {
            generator_error(ctx, st->lineno, "duplicate $repeat/$on_start");
            return 1;
        }
        st->task_armed          = 1;
        st->pending_interval_ms = 0L;
        return 1;
    }

    if (strncmp(line, "$repeat(", 8u) != 0) { return 0; }

    char value[GENERATOR_MAX_URL];
    if (parse_route_url(ctx, st, line, "$repeat", value) != 0) { return 1; }

    long interval = 0L;
    if (parse_interval(value, &interval) != 0)
    {
        generator_error(ctx, st->lineno, "$repeat requires an interval from 100ms to 24h, e.g. \"10s\" (got '%s')", value);
        return 1;
    }
    if (st->task_armed != 0)
    {
        generator_error(ctx, st->lineno, "duplicate $repeat/$on_start");
        return 1;
    }

    st->task_armed          = 1;
    st->pending_interval_ms = interval;

    return 1;
}

