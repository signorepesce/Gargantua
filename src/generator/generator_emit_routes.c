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

