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

void parse_task(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    st->task_armed = 0;

    if (ctx->task_count >= SCHEDULER_MAX_TASKS)
    {
        generator_error(ctx, st->lineno, "too many tasks (limit %d)", SCHEDULER_MAX_TASKS);
        return;
    }

    char function_name[GENERATOR_MAX_NAME];
    char return_type[GENERATOR_MAX_NAME];

    if ((extract_return_type(line, return_type, sizeof(return_type)) != 0) || (extract_function_name(line, function_name, sizeof(function_name)) != 0))
    {
        generator_error(ctx, st->lineno, "after $repeat expected 'void name(void)'");
        return;
    }
    if (strcmp(return_type, "void") != 0)
    {
        generator_error(ctx, st->lineno, "'%s' must return void", function_name);
        return;
    }

    ParsedRoute probe;
    memset(&probe, 0, sizeof(probe));
    if ((extract_params(ctx, st, line, &probe, 1) != 0) || (probe.param_count != 0))
    {
        generator_error(ctx, st->lineno, "'%s' takes no parameters", function_name);
        return;
    }

    ParsedTask *task = &ctx->tasks[ctx->task_count];
    memset(task, 0, sizeof(*task));
    (void)snprintf(task->function_name, sizeof(task->function_name), "%s", function_name);
    task->interval_ms = st->pending_interval_ms;
    task->line        = st->lineno;
    task->file_index  = ctx->file_count - 1;

    ctx->task_count++;

    if (ctx->file_count > 0) { ctx->files[ctx->file_count - 1].has_task = 1; }
}

int parse_auth_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (strcmp(line, "$authenticated") == 0)
    {
        st->pending_auth = 1;
        return 1;
    }
    if (strcmp(line, "$public") == 0)
    {
        st->pending_public = 1;
        return 1;
    }
    if (strncmp(line, "$role(", 6u) != 0) { return 0; }

    char role[GENERATOR_MAX_URL];
    if (parse_route_url(ctx, st, line, "$role", role) != 0) { return 1; }
    if ((strcmp(role, "admin") != 0) && (strcmp(role, "user") != 0))
    {
        generator_error(ctx, st->lineno, "$role supports admin or user");
        return 1;
    }

    (void)snprintf(st->pending_role, sizeof(st->pending_role), "%s", role);

    return 1;
}

int parse_transactional_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (word_equals(line, "$transactional") != 1) { return 0; }

    if (st->brace_depth != 0)
    {
        generator_error(ctx, st->lineno, "$transactional is allowed only before a function");
        return 1;
    }
    if (st->pending_transactional == 1)
    {
        generator_error(ctx, st->lineno, "duplicate $transactional");
        return 1;
    }

    st->pending_transactional = 1;

    return 1;
}

int arm_route_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    char head[GENERATOR_MAX_LINE];
    (void)snprintf(head, sizeof(head), "%s", line);

    char *paren = strchr(head, '(');
    if (paren == NULL) { return 0; }
    *paren = '\0';

    char       *word   = line_trim(head);
    const char *method = http_method_from_word(word);
    if (method == NULL) { return 0; }

    char url[GENERATOR_MAX_URL];
    if ((parse_route_url(ctx, st, line, word, url) != 0) || (validate_route_url(ctx, st, url) != 0)) { return 1; }

    (void)snprintf(st->route_method, sizeof(st->route_method), "%s", method);
    (void)snprintf(st->route_url, sizeof(st->route_url), "%s", url);
    st->route_armed = 1;

    return 1;
}

void open_type(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    char name[GENERATOR_MAX_NAME];
    if (parse_table_name(ctx, st->lineno, line, name, sizeof(name)) != 0) { return; }

    for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
    {
        if (word_equals(ctx->types[i].name, name) == 1)
        {
            generator_error(ctx, st->lineno, "table '%s' is already declared", name);
            return;
        }
    }

    if (ctx->type_count >= GENERATOR_MAX_TYPES)
    {
        generator_error(ctx, st->lineno, "too many tables (limit %d)", GENERATOR_MAX_TYPES);
        return;
    }

    st->current_type = &ctx->types[ctx->type_count];
    memset(st->current_type, 0, sizeof(*st->current_type));
    (void)snprintf(st->current_type->name, sizeof(st->current_type->name), "%s", name);
    st->current_type->json_only     = strncmp(line, "$json", 5u) == 0;
    st->current_type->line       = st->lineno;
    st->current_type->file_index = ctx->file_count - 1;

    st->in_struct = 1;
    st->need_open = 1;
    st->pending   = 0u;
    ctx->type_count++;

    if (ctx->file_count > 0) { ctx->files[ctx->file_count - 1].has_table = 1; }
}

void drop_dangling_auth(Generator *ctx, ParseState *st)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if ((st->pending_auth != 0) || (st->pending_public != 0) || (st->pending_role[0] != '\0'))
    {
        generator_error(ctx, st->lineno, "authentication annotation must precede a route");

        st->pending_auth    = 0;
        st->pending_public  = 0;
        st->pending_role[0] = '\0';
    }

    if (st->pending_produces[0] != '\0')
    {
        generator_error(ctx, st->lineno, "$produces must precede a route annotation");
        st->pending_produces[0] = '\0';
    }
}

int report_removed_annotation(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (strncmp(line, "$entity", 7u) == 0)
    {
        generator_error(ctx, st->lineno, "$entity was removed; use $table(Name)");
        return 1;
    }
    if (strncmp(line, "$dto", 4u) == 0)
    {
        generator_error(ctx, st->lineno, "$dto was removed; use $json(Name)");
        return 1;
    }

    return 0;
}
