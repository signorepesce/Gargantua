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

static void select_file_path(Generator *ctx, int file_index)
{
    assert(ctx != NULL);

    if ((file_index >= 0) && (file_index < ctx->file_count)) { (void)snprintf(ctx->path, sizeof(ctx->path), "%s", ctx->files[file_index].path); }
}

static void validate_route_types(Generator *ctx, const ParsedRoute *route)
{
    assert(ctx != NULL);
    assert(route != NULL);

    if ((word_equals(route->return_type, "str") == 0) && (type_exists(ctx, route->return_type) == 0)) { generator_error(ctx, route->line, "return type '%s' is not a declared $table", route->return_type); }

    for (int p = 0; (p < route->param_count) && (p < GENERATOR_MAX_PARAMS); p++)
    {
        const ParsedParam *param = &route->params[p];
        if ((param->is_body != 0) && (type_exists(ctx, param->type) == 0)) { generator_error(ctx, route->line, "body type '%s' is not a declared $table", param->type); }
    }
}

static void validate_route_clashes(Generator *ctx, int index)
{
    assert(ctx != NULL);

    const ParsedRoute *route = &ctx->routes[index];

    for (int prior = 0; prior < index; prior++)
    {
        const ParsedRoute *other = &ctx->routes[prior];

        if (word_equals(route->function_name, other->function_name) == 1) { generator_error(ctx, route->line, "handler '%s' already has a route annotation", route->function_name); }
        if ((word_equals(route->method, other->method) == 1) && (count_url_params(route->url) == count_url_params(other->url)) && (route_patterns_overlap(route->url, other->url) != 0)) { generator_error(ctx, route->line, "route %s %s is ambiguous with %s", route->method, route->url, other->url); }
    }
}

static void validate_routes(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->route_count) && (i < GENERATOR_MAX_ROUTES); i++)
    {
        const ParsedRoute *route = &ctx->routes[i];

        if ((strcmp(route->url, "/openapi.json") == 0) || (strcmp(route->url, "/health/live") == 0) || (strcmp(route->url, "/health/ready") == 0)) { generator_error(ctx, route->line, "/openapi.json is reserved for generated OpenAPI"); }

        select_file_path(ctx, route->file_index);
        validate_route_types(ctx, route);
        validate_route_clashes(ctx, i);
    }
}

static void validate_service_clashes(Generator *ctx, int index)
{
    assert(ctx != NULL);

    const ParsedService *service = &ctx->services[index];

    for (int prior = 0; prior < index; prior++)
    {
        if (word_equals(service->function_name, ctx->services[prior].function_name) == 1) { generator_error(ctx, service->line, "transactional service '%s' is declared twice", service->function_name); }
    }

    for (int route = 0; route < ctx->route_count; route++)
    {
        if (word_equals(service->function_name, ctx->routes[route].function_name) == 1) { generator_error(ctx, service->line, "'%s' cannot be both a service and an endpoint", service->function_name); }
    }
}

static void validate_services(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->service_count) && (i < GENERATOR_MAX_SERVICES); i++)
    {
        const ParsedService *service = &ctx->services[i];

        select_file_path(ctx, service->file_index);

        if (service_type_exists(ctx, service->return_type, 1) == 0) { generator_error(ctx, service->line, "service return type '%s' is not supported", service->return_type); }
        for (int p = 0; (p < service->param_count) && (p < GENERATOR_MAX_PARAMS); p++)
        {
            if (service_type_exists(ctx, service->params[p].type, 0) == 0) { generator_error(ctx, service->line, "service parameter type '%s' is not supported", service->params[p].type); }
        }

        validate_service_clashes(ctx, i);
    }
}

static void validate_stores(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->store_count) && (i < GENERATOR_MAX_STORES); i++)
    {
        const ParsedStore *store = &ctx->stores[i];

        select_file_path(ctx, store->file_index);

        int index = type_index(ctx, store->type);
        if (index < 0)
        {
            generator_error(ctx, store->line, "$store type '%s' is not a declared $table or $json", store->type);
            continue;
        }
        const ParsedType *type = &ctx->types[index];
        for (int field = 0; field < type->field_count; field++)
        {
            if (word_equals(type->fields[field].kind, "FIELD_OBJECT")) { generator_error(ctx, store->line, "$store(%s) requires flat fields; nested field '%s' is unsupported", store->type, type->fields[field].name); }
        }
    }
}

static void validate_tasks(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->task_count) && (i < SCHEDULER_MAX_TASKS); i++)
    {
        const ParsedTask *task = &ctx->tasks[i];

        select_file_path(ctx, task->file_index);

        for (int prior = 0; prior < i; prior++)
        {
            if (word_equals(task->function_name, ctx->tasks[prior].function_name) == 1) { generator_error(ctx, task->line, "task '%s' is declared twice", task->function_name); }
        }
        for (int r = 0; r < ctx->route_count; r++)
        {
            if (word_equals(task->function_name, ctx->routes[r].function_name) == 1) { generator_error(ctx, task->line, "'%s' cannot be both a task and an endpoint", task->function_name); }
        }
    }
}

static void validate_file_roles(Generator *ctx)
{
    assert(ctx != NULL);

    for (int i = 0; i < ctx->file_count; i++)
    {
        const SourceFile *input = &ctx->files[i];

        if (input->has_service && (input->has_table || input->has_route))
        {
            (void)snprintf(ctx->path, sizeof(ctx->path), "%s", input->path);
            generator_error(ctx, 0, "put transactional services in a separate service file");
        }
    }
}

void generator_validate_model(Generator *ctx)
{
    assert(ctx != NULL);

    validate_types(ctx);

    char saved_path[GENERATOR_MAX_PATH];
    (void)snprintf(saved_path, sizeof(saved_path), "%s", ctx->path);

    validate_routes(ctx);
    validate_services(ctx);
    validate_stores(ctx);
    validate_tasks(ctx);
    validate_file_roles(ctx);

    (void)snprintf(ctx->path, sizeof(ctx->path), "%s", saved_path);
}
