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

static void check_validation(Generator *ctx, ParseState *st, ParsedField *f)
{
    assert(ctx != NULL);
    assert(f != NULL);

    int numeric = (strcmp(f->kind, "FIELD_INT") == 0) ||
                  (strcmp(f->kind, "FIELD_LONG") == 0) ||
                  (strcmp(f->kind, "FIELD_DOUBLE") == 0);

    if ((((f->validation & 3u) != 0u) && (numeric == 0)) || (((f->validation & 12u) != 0u) && (strcmp(f->kind, "FIELD_STR") != 0)) || (((f->validation & 3u) == 3u) && (f->min > f->max))) { generator_error(ctx, st->lineno, "validation does not match field type or Min exceeds Max"); }

    int integral = (strcmp(f->kind, "FIELD_INT") == 0) ||
                   (strcmp(f->kind, "FIELD_LONG") == 0);

    if (integral != 0)
    {
        check_integral_bounds(ctx, st, f);
        return;
    }

    if ((((f->validation & 1u) != 0u) && ((f->min < -DBL_MAX) || (f->min > DBL_MAX))) || (((f->validation & 2u) != 0u) && ((f->max < -DBL_MAX) || (f->max > DBL_MAX)))) { generator_error(ctx, st->lineno, "validation bound is outside the field type range"); }
}

static int field_conflicts(Generator *ctx, ParseState *st, const ParsedField *f)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(f != NULL);

    if (((f->flags & 1u) != 0u) && (strcmp(f->c_type, "int") != 0))
    {
        generator_error(ctx, st->lineno, "$Id must be int for a safe auto-id");
        return 1;
    }

    for (int i = 0; (i < st->current_type->field_count) && (i < GENERATOR_MAX_FIELDS); i++)
    {
        const ParsedField *prev = &st->current_type->fields[i];

        if (word_equals(prev->name, f->name) == 1)
        {
            generator_error(ctx, st->lineno, "field '%s' is declared twice", f->name);
            return 1;
        }
        if (((prev->flags & 1u) != 0u) && ((f->flags & 1u) != 0u))
        {
            generator_error(ctx, st->lineno, "only one $Id per table is allowed");
            return 1;
        }
    }

    return 0;
}

static int field_line_ready(Generator *ctx, ParseState *st, char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    size_t len = strlen(line);
    if ((len == 0u) || (line[len - 1u] != ';'))
    {
        if (line_only_annotations(line, &st->pending) == 0) { generator_error(ctx, st->lineno, "missing ';' at the end of the field"); }
        return 0;
    }

    line[len - 1u] = '\0';

    if (st->current_type == NULL)
    {
        generator_error(ctx, st->lineno, "field outside a struct");
        return 0;
    }
    if (st->current_type->field_count >= GENERATOR_MAX_FIELDS)
    {
        generator_error(ctx, st->lineno, "too many fields (limit %d)", GENERATOR_MAX_FIELDS);
        st->pending = 0u;
        return 0;
    }

    return 1;
}

static void parse_type_body_line(Generator *ctx, ParseState *st, char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (rule_annotation(ctx, st, line)) { return; }
    if (parse_references_annotation(ctx, st, line) != 0) { return; }
    if (field_line_ready(ctx, st, line) == 0) { return; }

    ParsedField f;
    if (parse_field(ctx, line, st->lineno, &f) == 1)
    {
        apply_pending_rules(&f, st);
        check_validation(ctx, st, &f);

        (void)snprintf(f.references, sizeof(f.references), "%s", st->pending_references);
        st->pending_references[0] = '\0';

        if (field_conflicts(ctx, st, &f) != 0)
        {
            st->pending = 0u;
            return;
        }

        st->current_type->fields[st->current_type->field_count] = f;
        st->current_type->field_count++;
    }

    st->pending = 0u;
}

static void parse_source_line(Generator *ctx, ParseState *st, char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(line != NULL);

    if (st->in_struct == 0)
    {
        guard_transaction(ctx, st, line);
        parse_top_level_line(ctx, st, line);
        return;
    }

    if (line[0] == '\0') { return; }

    if (st->need_open != 0)
    {
        if (strcmp(line, "{") != 0)
        {
            generator_error(ctx, st->lineno, "after $table(...) expected '{' on its own");
        }
        st->need_open = 0;
        return;
    }

    if (line[0] == '}')
    {
        handle_close(ctx, st, line);
        return;
    }

    parse_type_body_line(ctx, st, line);
}

static void report_unfinished(Generator *ctx, const ParseState *st, int truncated)
{
    assert(ctx != NULL);
    assert(st != NULL);

    if (truncated != 0) { generator_error(ctx, st->lineno, "the file exceeds %d lines", GENERATOR_MAX_LINES); }
    if (st->in_block_comment != 0) { generator_error(ctx, st->lineno, "the block comment was not closed"); }
    if (st->route_armed != 0) { generator_error(ctx, st->lineno, "the handler signature after the route annotation is missing"); }
    if (st->pending_auth || st->pending_public || st->pending_role[0]) { generator_error(ctx, st->lineno, "authentication annotation without endpoint"); }
    if (st->pending_produces[0] != '\0') { generator_error(ctx, st->lineno, "$produces is not followed by a route"); }
    if (st->task_armed != 0) { generator_error(ctx, st->lineno, "$repeat/$on_start is not followed by a function"); }
    if (st->pending_transactional == 1) { generator_error(ctx, st->lineno, "$transactional at the end of the file is not" " followed by a service or an endpoint"); }
    if (st->in_struct == 1)
    {
        generator_error(ctx, st->lineno, (st->need_open != 0) ? "the table's '{' is missing" : "the table was not closed with '};'");
    }
}

void generator_parse_source(Generator *ctx, char *src, size_t len)
{
    assert(ctx != NULL);
    assert(src != NULL);
    assert(len <= (size_t)GENERATOR_MAX_SOURCE);

    if (memchr(src, '\0', len) != NULL)
    {
        generator_error(ctx, 0, "the file contains a NUL byte");
        return;
    }

    ParseState st;
    memset(&st, 0, sizeof(st));

    char *p = src;
    for (int i = 0; i < GENERATOR_MAX_LINES; i++)
    {
        if (*p == '\0') { break; }

        char  *start = p;
        char  *eol   = strchr(p, '\n');
        size_t n     = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        st.lineno++;
        p = (eol != NULL) ? (eol + 1) : (p + n);

        if (n >= (size_t)GENERATOR_MAX_LINE)
        {
            generator_error(ctx, st.lineno, "line longer than %d bytes", GENERATOR_MAX_LINE - 1);
            continue;
        }

        char work[GENERATOR_MAX_LINE];
        memcpy(work, start, n);
        work[n] = '\0';

        strip_comments(work, &st.in_block_comment);
        parse_source_line(ctx, &st, line_trim(work));
    }

    report_unfinished(ctx, &st, (*p != '\0') ? 1 : 0);
}
