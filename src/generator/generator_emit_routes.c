#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "generator.h"
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "emit_internal.h"

static void emit_transaction_close(FILE *out, const ParsedRoute *r)
{
    (void)r;
    (void)fprintf(out, "    if (request_failed()) { return 0; }\n");
}

static void emit_wrapper_unused(FILE *out, const ParsedRoute *r)
{
    assert(out != NULL);
    assert(r != NULL);

    int scalars = 0;
    int bodies  = 0;

    for (int k = 0; (k < r->param_count) && (k < GENERATOR_MAX_PARAMS); k++)
    {
        if (r->params[k].is_body == 1) { bodies++; } else { scalars++; }
    }

    if (scalars == 0) { (void)fprintf(out, "    (void)wrap_p;\n"); }
    if (bodies == 0) { (void)fprintf(out, "    (void)wrap_body;\n    (void)wrap_body_len;\n"); }
}

static void emit_wrapper_param(FILE *out, const ParsedRoute *r, int k)
{
    assert(out != NULL);
    assert(r != NULL);

    const ParsedParam *prm = &r->params[k];

    if (prm->is_body == 1)
    {
        (void)fprintf(out, "    %s %s = {0};\n" "    if (json_read_mode(&%s__type, &%s, wrap_body, wrap_body_len, %d)" " != 0)\n    {\n        return -2;\n    }\n", prm->type, prm->name, prm->type, prm->name, strcmp(r->method, "PATCH") == 0);
        return;
    }

    if (prm->is_query == 1)
    {
        if (strcmp(prm->type, "int") == 0)
        {
            (void)fprintf(out, "    int wrap_ok_%d = 0;\n" "    int %s = query_int(wrap_p, \"%s\", &wrap_ok_%d);\n" "    if (wrap_ok_%d == 0)\n    {\n" "        return -2;\n    }\n", k, prm->name, prm->name, k, k);
        }
        else
        {
            (void)fprintf(out, "    str %s = query_text(wrap_p, \"%s\");\n", prm->name, prm->name);
        }
        return;
    }

    if (strcmp(prm->type, "int") == 0)
    {
        (void)fprintf(out, "    int wrap_ok_%d = 0;\n" "    int %s = path_param_int(wrap_p, %d, &wrap_ok_%d);\n" "    if (wrap_ok_%d == 0)\n    {\n" "        return -2;\n    }\n", k, prm->name, prm->path_slot, k, k);
        return;
    }

    (void)fprintf(out, "    str %s = path_param_text(wrap_p, %d);\n", prm->name, prm->path_slot);
}

static void emit_wrapper_call(FILE *out, const ParsedRoute *r)
{
    assert(out != NULL);
    assert(r != NULL);

    if (r->collection != 0)
    {
        (void)fprintf(out, "    RowList wrap_value = %s(", r->function_name);
    }
    else if (strcmp(r->return_type, "str") == 0)
    {
        (void)fprintf(out, "    str wrap_value = %s(", r->function_name);
    }
    else
    {
        (void)fprintf(out, "    %s wrap_value = %s(", r->return_type, r->function_name);
    }

    for (int k = 0; (k < r->param_count) && (k < GENERATOR_MAX_PARAMS); k++) { (void)fprintf(out, "%s%s", (k > 0) ? ", " : "", r->params[k].name); }

    (void)fprintf(out, ");\n");
    emit_transaction_close(out, r);
}

static void emit_wrapper_result(FILE *out, const ParsedRoute *r)
{
    assert(out != NULL);
    assert(r != NULL);

    if (r->collection != 0)
    {
        (void)fprintf(out, "    if (wrap_value.type != &%s__type) { return -1; }\n" "    int wrap_result = row_list_write(wrap_value, %d, wrap_out, wrap_cap);\n", r->return_type, (r->collection == 2) ? 1 : 0);
        return;
    }

    if (strcmp(r->return_type, "str") == 0)
    {
        (void)fprintf(out, "    int wrap_result = text_write(wrap_value, wrap_out," " wrap_cap);\n");
        return;
    }

    (void)fprintf(out, "    int wrap_result = json_write_struct(&%s__type, &wrap_value," " wrap_out, wrap_cap);\n", r->return_type);
}

static void emit_route_wrapper(FILE *out, const ParsedRoute *r)
{
    assert(out != NULL);
    assert(r != NULL);

    (void)fprintf(out, "static int wrap_%s(const RequestParams *wrap_p," " char *wrap_body, size_t wrap_body_len,\n" "                       char *wrap_out, size_t wrap_cap)" "\n{\n", r->function_name);

    (void)fprintf(out, "    if (auth_gate(%d, \"%s\", %d) != 0) { return 0; }\n", r->public_route, r->role, r->authenticated);

    emit_wrapper_unused(out, r);

    for (int k = 0; (k < r->param_count) && (k < GENERATOR_MAX_PARAMS); k++) { emit_wrapper_param(out, r, k); }

    if (r->transactional == 1)
    {
        (void)fprintf(out, "    Transaction wrap_scope __attribute__((cleanup(transaction_cleanup)))" " = transaction_begin();\n" "    if (!wrap_scope.active) { return 0; }\n");
    }

    emit_wrapper_call(out, r);
    emit_wrapper_result(out, r);

    if (r->transactional == 1)
    {
        (void)fprintf(out, "    if (wrap_result != 0) { request_fail(500, \"serialization failed\"); }\n" "    transaction_end(&wrap_scope);\n");
    }

    (void)fprintf(out, "    return wrap_result;\n}\n\n");
}

void emit_route_wrappers(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->route_count) && (i < GENERATOR_MAX_ROUTES); i++) { emit_route_wrapper(out, &ctx->routes[i]); }
}

static int dynamic_segments(const char *url)
{
    int count = 0;
    for (size_t i = 0u; (i < GENERATOR_MAX_URL) && (url[i] != '\0'); i++)
    {
        if (url[i] == '{') { count++; }
    }
    return count;
}

static int store_text_field_count(const Generator *ctx, const char *type_name)
{
    assert(ctx != NULL);
    assert(type_name != NULL);

    for (int i = 0; i < ctx->type_count; i++)
    {
        if (strcmp(ctx->types[i].name, type_name) != 0) { continue; }

        int fields = 0;
        for (int f = 0; f < ctx->types[i].field_count; f++)
        {
            if (strcmp(ctx->types[i].fields[f].kind, "FIELD_STR") == 0) { fields++; }
        }
        return fields;
    }

    return 0;
}

void emit_store_declarations(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->store_count) && (i < GENERATOR_MAX_STORES); i++)
    {
        const char *type = ctx->stores[i].type;

        (void)fprintf(out, "int     %s_store_count(void);\n", type);
        (void)fprintf(out, "int     %s_store_capacity(void);\n", type);
        (void)fprintf(out, "int     %s_store_add(%s wrap_value);\n", type, type);
        (void)fprintf(out, "int     %s_store_put(int wrap_index, %s wrap_value);\n", type, type);
        (void)fprintf(out, "%s %s_store_at(int wrap_index);\n", type, type);
        (void)fprintf(out, "RowList %s_store_list(void);\n", type);
        (void)fprintf(out, "void    %s_store_clear(void);\n", type);
    }
}

void emit_stores(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->store_count) && (i < GENERATOR_MAX_STORES); i++)
    {
        const char *type     = ctx->stores[i].type;
        int         capacity = ctx->stores[i].capacity;
        int         texts    = store_text_field_count(ctx, type);

        char text_expr[GENERATOR_MAX_NAME + 8];
        (void)snprintf(text_expr, sizeof(text_expr), "(char *)0");

        (void)fprintf(out, "\nstatic %s %s_ROWS[%d];\n", type, type, capacity);

        if (texts > 0)
        {
            (void)fprintf(out, "static char %s_TEXT[%d * %d * STORE_TEXT_LEN];\n", type, capacity, texts);
            (void)snprintf(text_expr, sizeof(text_expr), "%s_TEXT", type);
        }

        (void)fprintf(out, "static Store %s_STORE =\n{\n" "    .type = &%s__type,\n" "    .rows = %s_ROWS,\n" "    .text = %s,\n" "    .capacity = %d,\n" "    .count = 0,\n" "    .text_fields = %d,\n" "    .lock = PTHREAD_MUTEX_INITIALIZER\n" "};\n\n", type, type, type, text_expr, capacity, texts);

        (void)fprintf(out, "int     %s_store_count(void) { return store_count(&%s_STORE); }\n", type, type);
        (void)fprintf(out, "int     %s_store_capacity(void) { return %d; }\n", type, capacity);
        (void)fprintf(out, "int     %s_store_add(%s wrap_value)" " { return store_add(&%s_STORE, &wrap_value); }\n", type, type, type);
        (void)fprintf(out, "int     %s_store_put(int wrap_index, %s wrap_value)" " { return store_put(&%s_STORE, wrap_index, &wrap_value); }\n", type, type, type);
        (void)fprintf(out, "%s %s_store_at(int wrap_index)\n{\n" "    %s wrap_out = {0};\n" "    (void)store_get(&%s_STORE, wrap_index, &wrap_out);\n" "    return wrap_out;\n}\n", type, type, type, type);
        (void)fprintf(out, "RowList %s_store_list(void) { return store_list(&%s_STORE); }\n", type, type);
        (void)fprintf(out, "void    %s_store_clear(void) { store_clear(&%s_STORE); }\n", type, type);
    }
}

void emit_task_table(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    if (ctx->task_count <= 0)
    {
        (void)fprintf(out, "\nconst Task *task_table(void) { return (const Task *)0; }\n" "int task_count(void) { return 0; }\n");
        return;
    }

    (void)fprintf(out, "\n");

    (void)fprintf(out, "\nstatic const Task TASKS[] =\n{\n");
    for (int i = 0; (i < ctx->task_count) && (i < SCHEDULER_MAX_TASKS); i++)
    {
        (void)fprintf(out, "    { \"%s\", %ldL, %s },\n", ctx->tasks[i].function_name, ctx->tasks[i].interval_ms, ctx->tasks[i].function_name);
    }
    (void)fprintf(out, "};\n\n" "const Task *task_table(void) { return TASKS; }\n" "int task_count(void) { return (int)(sizeof(TASKS) / sizeof(TASKS[0])); }\n");
}

void emit_route_table(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    (void)fprintf(out, "static const Route ROUTES[] =\n{\n");
    (void)fprintf(out, "    { \"GET\", \"/openapi.json\", \"application/json; charset=utf-8\", 200, wrap_openapi },\n");
    int emitted[GENERATOR_MAX_ROUTES];
    memset(emitted, 0, sizeof(emitted));

    for (int pos = 0; (pos < ctx->route_count) && (pos < GENERATOR_MAX_ROUTES); pos++)
    {
        int best = -1;
        int best_dynamic = INT_MAX;
        for (int i = 0; (i < ctx->route_count) && (i < GENERATOR_MAX_ROUTES); i++)
        {
            int dynamic = dynamic_segments(ctx->routes[i].url);
            if ((emitted[i] == 0) && (dynamic < best_dynamic))
            {
                best = i;
                best_dynamic = dynamic;
            }
        }
        assert(best >= 0);
        emitted[best] = 1;
        int i = best;
        const ParsedRoute *r = &ctx->routes[i];
        int text = (strcmp(r->return_type, "str") == 0) ? 1 : 0;

        int created = (strcmp(r->method, "POST") == 0) ? 201 : 200;

        const char *content_type = (r->produces[0] != '\0') ? r->produces
                                 : ((text == 1) ? "text/plain; charset=utf-8" : "application/json; charset=utf-8");

        (void)fprintf(out, "    { \"%s\", \"%s\", \"%s\", %d, wrap_%s },\n", r->method, r->url, content_type, created, r->function_name);
    }
    (void)fprintf(out, "};\n\n" "const Route *route_table(void) { return ROUTES; }\n");
    {
        (void)fprintf(out, "int route_count(void)" " { return (int)(sizeof(ROUTES) / sizeof(ROUTES[0])); }\n");
    }
}
