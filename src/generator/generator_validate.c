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

static int type_exists(const Generator *ctx, const char *name)
{
    for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
    {
        if (word_equals(ctx->types[i].name, name) == 1) { return 1; }
    }
    return 0;
}

static int service_type_exists(const Generator *ctx, const char *name, int allow_void)
{
    assert(ctx != NULL);
    assert(name != NULL);

    if (word_equals(name, "int") == 1 || word_equals(name, "long") == 1 || word_equals(name, "double") == 1 || word_equals(name, "bool") == 1 || word_equals(name, "str") == 1) { return 1; }
    if (allow_void && word_equals(name, "void") == 1) { return 1; }
    return type_exists(ctx, name);
}

static int route_patterns_overlap(const char *a, const char *b)
{
    const char *pa = a + 1;
    const char *pb = b + 1;

    for (int guard = 0; guard < GENERATOR_MAX_ROUTE_SEGMENTS; guard++)
    {
        if ((*pa == '\0') || (*pb == '\0')) { return ((*pa == '\0') && (*pb == '\0')) ? 1 : 0; }

        const char *sa = strchr(pa, '/');
        const char *sb = strchr(pb, '/');
        size_t na = (sa != NULL) ? (size_t)(sa - pa) : strlen(pa);
        size_t nb = (sb != NULL) ? (size_t)(sb - pb) : strlen(pb);
        int dynamic_a = (pa[0] == '{' && pa[na - 1u] == '}') ? 1 : 0;
        int dynamic_b = (pb[0] == '{' && pb[nb - 1u] == '}') ? 1 : 0;

        if ((dynamic_a == 0) && (dynamic_b == 0) && ((na != nb) || (memcmp(pa, pb, na) != 0))) { return 0; }
        if ((sa == NULL) || (sb == NULL)) { return ((sa == NULL) && (sb == NULL)) ? 1 : 0; }
        pa = sa + 1;
        pb = sb + 1;
    }
    return 0;
}

static int type_index(const Generator *ctx, const char *name)
{
    for (int i = 0; i < ctx->type_count; i++)
    {
        if (strcmp(ctx->types[i].name, name) == 0) { return i; }
    }
    return -1;
}

static void validate_nested_field(Generator *ctx, ParsedType *t, int type_slot, const ParsedField *f)
{
    assert(ctx != NULL);
    assert(t != NULL);
    assert(f != NULL);

    int target = type_index(ctx, f->c_type);

    if ((target < 0) || (t->json_only == 0) || ((f->flags & ~2u) != 0u))
    {
        generator_error(ctx, t->line, "nested field %s.%s needs a declared type inside $json;" " only $NotNull is allowed", t->name, f->name);
        return;
    }

    if ((ctx->types[target].file_index == t->file_index) && (target >= type_slot)) { generator_error(ctx, t->line, "declare nested type %s before %s in the same file", f->c_type, t->name); }
}

static void validate_reference_field(Generator *ctx, const ParsedType *t, ParsedField *f)
{
    assert(ctx != NULL);
    assert(t != NULL);
    assert(f != NULL);

    int target = type_index(ctx, f->references);
    int key    = -1;

    if (target >= 0)
    {
        for (int k = 0; k < ctx->types[target].field_count; k++)
        {
            if ((ctx->types[target].fields[k].flags & 1u) != 0u) { key = k; }
        }
    }

    if ((t->json_only != 0) || (strcmp(f->c_type, "int") != 0) || (target < 0) || (key < 0) || (ctx->types[target].json_only != 0))
    {
        generator_error(ctx, t->line, "$References requires an int field and a table with $Id");
        return;
    }

    (void)snprintf(f->reference_key, sizeof(f->reference_key), "%s", ctx->types[target].fields[key].name);
}

static void validate_type_fields(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; i < ctx->type_count; i++)
    {
        ParsedType *t = &ctx->types[i];

        for (int j = 0; j < t->field_count; j++)
        {
            ParsedField *f = &t->fields[j];

            if (strcmp(f->kind, "FIELD_OBJECT") == 0) { validate_nested_field(ctx, t, i, f); }
            if (f->references[0] != '\0') { validate_reference_field(ctx, t, f); }
            if ((t->json_only != 0) && ((f->flags & 5u) != 0u)) { generator_error(ctx, t->line, "$Id/$Unique belong on $table fields"); }
        }
    }
}

static void validate_nesting_depth(Generator *ctx)
{
    assert(ctx != NULL);

    int depth[GENERATOR_MAX_TYPES] = {0};

    for (int pass = 0; pass < ctx->type_count; pass++)
    {
        for (int i = 0; i < ctx->type_count; i++)
        {
            if (depth[i] != 0) { continue; }

            int value = 1;
            for (int j = 0; j < ctx->types[i].field_count; j++)
            {
                const ParsedField *f = &ctx->types[i].fields[j];
                if (strcmp(f->kind, "FIELD_OBJECT") != 0) { continue; }

                int k = type_index(ctx, f->c_type);
                if ((k < 0) || (depth[k] == 0)) { value = 0; break; }
                if ((depth[k] + 1) > value) { value = depth[k] + 1; }
            }
            depth[i] = value;
        }
    }

    for (int i = 0; i < ctx->type_count; i++)
    {
        if ((depth[i] == 0) || (depth[i] > 8)) { generator_error(ctx, ctx->types[i].line, "nested type cycle or depth exceeds 8: %s", ctx->types[i].name); }
    }
}

static void validate_types(Generator *ctx)
{
    assert(ctx != NULL);

    validate_type_fields(ctx);
    validate_nesting_depth(ctx);
}

