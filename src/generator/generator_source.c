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

static void parse_top_level_line(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (line[0] == '\0') { return; }
    if (parse_auth_annotation(ctx, st, line) != 0) { return; }
    if (parse_produces_annotation(ctx, st, line) != 0) { return; }
    if (st->route_armed == 1)
    {
        parse_route(ctx, st, line);
        return;
    }
    if (parse_store_annotation(ctx, st, line) != 0) { return; }
    if (parse_repeat_annotation(ctx, st, line) != 0) { return; }
    if (st->task_armed == 1)
    {
        parse_task(ctx, st, line);
        return;
    }
    if (parse_transactional_annotation(ctx, st, line) != 0) { return; }
    if (arm_route_annotation(ctx, st, line) != 0) { return; }

    drop_dangling_auth(ctx, st);

    if (st->pending_transactional == 1)
    {
        parse_service(ctx, st, line);
        return;
    }

    if (report_removed_annotation(ctx, st, line) != 0) { return; }

    if ((strncmp(line, "$table", 6u) == 0) || (strncmp(line, "$json", 5u) == 0)) { open_type(ctx, st, line); }
}

static void handle_close(Generator *ctx, ParseState *st, char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (st->current_type == NULL)
    {
        generator_error(ctx, st->lineno, "table closed without being opened");
    }
    else
    {
        if (strcmp(line, "};") != 0)
        {
            generator_error(ctx, st->lineno, "close the table exactly with '};'");
        }
        if (st->pending != 0u || st->pending_references[0] != '\0' || st->rules.validation != 0u)
        {
            generator_error(ctx, st->lineno, "field annotation with no field before '};'");
        }
        if (st->current_type->field_count == 0) { generator_error(ctx, st->lineno, "the table has no fields"); }
    }

    st->in_struct = 0;
    st->need_open = 0;
    st->current_type       = NULL;
    st->pending   = 0u;
}

static int rule_annotation(Generator *ctx, ParseState *st, const char *line)
{
    unsigned flag = 0u;
    const char *args = NULL;
    if (strncmp(line, "$Min(", 5u) == 0) { flag = 1u; args = line + 5; }
    else if (strncmp(line, "$Max(", 5u) == 0) { flag = 2u; args = line + 5; }
    else if (strncmp(line, "$Size(", 6u) == 0) { flag = 4u; args = line + 6; }
    else if (strcmp(line, "$Email") == 0) { flag = 8u; }
    else { return 0; }
    int invalid = (st->rules.validation & flag) != 0u;
    if (args != NULL)
    {
        char *end = NULL;
        errno = 0;
        long double value = strtold(args, &end);
        if (errno || end == args || !isfinite(value)) { invalid = 1; }
        if (flag == 1u || flag == 2u)
        {
            size_t n = (size_t)(end - args);
            if (n >= GENERATOR_MAX_NAME) { invalid = 1; }
            else
            {
                char *target = flag == 1u ? st->rules.min_arg : st->rules.max_arg;
                memcpy(target, args, n); target[n] = '\0';
            }
        }
        while (*end == ' ' || *end == '\t') { end++; }
        if (flag == 4u)
        {
            if (*end != ',') { invalid = 1; }
            else
            {
                const char *second = end + 1;
                errno = 0;
                long double max = strtold(second, &end);
                if (errno || end == second || !isfinite(max) || value < 0 || max < value || max > 1048576 || value != (unsigned)value || max != (unsigned)max)
                { invalid = 1; }
                if (!invalid) { st->rules.size_min = (unsigned)value; st->rules.size_max = (unsigned)max; }
                while (*end == ' ' || *end == '\t') { end++; }
            }
        }
        else if (flag == 1u) { st->rules.min = value; }
        else { st->rules.max = value; }
        if (*end != ')' || end[1] != '\0') { invalid = 1; }
    }
    if (invalid) { generator_error(ctx, st->lineno, "invalid or duplicate validation annotation: %s", line); }
    else { st->rules.validation |= flag; }
    return 1;
}

static int parse_references_annotation(Generator *ctx, ParseState *st, char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (strncmp(line, "$References(", 12u) != 0) { return 0; }

    size_t n = strlen(line);
    if ((n < 14u) || (line[n - 1u] != ')') || ((n - 13u) >= GENERATOR_MAX_NAME) || (st->pending_references[0] != '\0'))
    {
        generator_error(ctx, st->lineno, "use one $References(Table) before an int field");
        return 1;
    }

    line[n - 1u] = '\0';
    if (is_safe_identifier(line + 12) == 0)
    {
        generator_error(ctx, st->lineno, "invalid reference target");
        return 1;
    }

    (void)snprintf(st->pending_references, sizeof(st->pending_references), "%s", line + 12);

    return 1;
}

static void apply_pending_rules(ParsedField *f, ParseState *st)
{
    assert(f != NULL);
    assert(st != NULL);

    f->flags     |= st->pending;
    f->validation = st->rules.validation;
    f->min        = st->rules.min;
    f->max        = st->rules.max;
    f->size_min   = st->rules.size_min;
    f->size_max   = st->rules.size_max;

    memcpy(f->min_arg, st->rules.min_arg, sizeof(f->min_arg));
    memcpy(f->max_arg, st->rules.max_arg, sizeof(f->max_arg));
    memset(&st->rules, 0, sizeof(st->rules));
}

static void check_integral_bounds(Generator *ctx, ParseState *st, ParsedField *f)
{
    assert(ctx != NULL);
    assert(f != NULL);

    for (unsigned rule = 1u; rule <= 2u; rule++)
    {
        if ((f->validation & rule) == 0u) { continue; }

        const char *arg = (rule == 1u) ? f->min_arg : f->max_arg;
        char       *end = NULL;

        errno = 0;
        long bound = strtol(arg, &end, 10);

        if ((errno != 0) || (end == arg) || (*end != '\0') || ((strcmp(f->kind, "FIELD_INT") == 0) && ((bound < INT_MIN) || (bound > INT_MAX)))) { generator_error(ctx, st->lineno, "integer validation needs a decimal integer within the field type range"); }

        if (rule == 1u) { f->min_int = bound; } else { f->max_int = bound; }
    }

    if (((f->validation & 3u) == 3u) && (f->min_int > f->max_int)) { generator_error(ctx, st->lineno, "Min exceeds Max"); }
}

