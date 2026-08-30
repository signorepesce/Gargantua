#include "generator.h"
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "emit_internal.h"

static void emit_service_params(FILE *out, const ParsedService *service, int names)
{
    assert(out != NULL);
    assert(service != NULL);

    if (service->param_count == 0)
    {
        (void)fprintf(out, "void");
        return;
    }
    for (int i = 0; i < service->param_count && i < GENERATOR_MAX_PARAMS; i++)
    {
        const ParsedParam *param = &service->params[i];
        (void)fprintf(out, "%s%s%s", i ? ", " : "", names ? "" : param->type, names ? param->name : "");
        if (!names) { (void)fprintf(out, " %s", param->name); }
    }
}

static const char *service_name_in_line(const char *line, const char *name)
{
    assert(line != NULL);
    assert(name != NULL);

    size_t length = strlen(name);
    const char *cursor = line;
    for (int guard = 0; guard < GENERATOR_MAX_LINE; guard++)
    {
        const char *found = strstr(cursor, name);
        if (found == NULL) { return NULL; }
        unsigned char before = found == line ? 0u : (unsigned char)found[-1];
        const char *after = found + length;
        while (*after == ' ' || *after == '\t') { after++; }
        if ((found == line || (!isalnum(before) && before != '_')) && *after == '(') { return found; }
        cursor = found + length;
    }
    return NULL;
}

static int emit_service_input(FILE *out, const Generator *ctx, int file_index)
{
    assert(out != NULL);
    assert(ctx != NULL);
    assert(file_index >= 0 && file_index < ctx->file_count);

    FILE *source = fopen(ctx->files[file_index].path, "r");
    if (source == NULL) { return -1; }
    char line[GENERATOR_MAX_LINE];
    int line_number = 0;
    int transformed[GENERATOR_MAX_SERVICES] = {0};
    int result = 0;
    while (fgets(line, (int)sizeof(line), source) != NULL)
    {
        line_number++;
        const ParsedService *match = NULL;
        int match_index = -1;
        for (int i = 0; i < ctx->service_count && i < GENERATOR_MAX_SERVICES; i++)
        {
            if (ctx->services[i].file_index == file_index && ctx->services[i].line == line_number)
            {
                match = &ctx->services[i];
                match_index = i;
                break;
            }
        }
        if (match == NULL)
        {
            if (fputs(line, out) == EOF) { result = -1; break; }
            continue;
        }
        const char *name = service_name_in_line(line, match->function_name);
        if (name == NULL) { result = -1; break; }
        size_t prefix = (size_t)(name - line);
        if (fputs("static ", out) == EOF || fwrite(line, 1u, prefix, out) != prefix || fprintf(out, "wrap_service_%s%s", match->function_name, name + strlen(match->function_name)) < 0)
        {
            result = -1;
            break;
        }
        transformed[match_index] = 1;
    }
    if (ferror(source) != 0) { result = -1; }
    if (fclose(source) != 0) { result = -1; }
    for (int i = 0; i < ctx->service_count && i < GENERATOR_MAX_SERVICES; i++)
    {
        if (ctx->services[i].file_index == file_index && !transformed[i]) { result = -1; }
    }
    return result;
}

static void emit_service_wrapper(FILE *out, const ParsedService *service)
{
    assert(out != NULL);
    assert(service != NULL);

    (void)fprintf(out, "\n%s %s(", service->return_type, service->function_name);
    emit_service_params(out, service, 0);
    (void)fprintf(out, ")\n{\n" "    Transaction wrap_scope __attribute__((cleanup(transaction_cleanup))) = transaction_begin();\n");
    if (strcmp(service->return_type, "void") == 0)
    {
        (void)fprintf(out, "    if (!wrap_scope.active)\n" "    {\n" "        return;\n" "    }\n" "    wrap_service_%s(", service->function_name);
        emit_service_params(out, service, 1);
        (void)fprintf(out, ");\n" "    transaction_end(&wrap_scope);\n" "}\n");
        return;
    }
    (void)fprintf(out, "    %s wrap_value = {0};\n" "    if (!wrap_scope.active)\n" "    {\n" "        return wrap_value;\n" "    }\n" "    wrap_value = wrap_service_%s(", service->return_type, service->function_name);
    emit_service_params(out, service, 1);
    (void)fprintf(out, ");\n" "    transaction_end(&wrap_scope);\n" "    return wrap_value;\n" "}\n");
}

static int model_file_ready(const Generator *ctx, int file, const int *included)
{
    assert(ctx != NULL);
    assert(included != NULL);

    for (int t = 0; t < ctx->type_count; t++)
    {
        if (ctx->types[t].file_index != file) { continue; }

        for (int f = 0; f < ctx->types[t].field_count; f++)
        {
            const ParsedField *field = &ctx->types[t].fields[f];
            if (strcmp(field->kind, "FIELD_OBJECT") != 0) { continue; }

            for (int k = 0; k < ctx->type_count; k++)
            {
                int dependency = ctx->types[k].file_index;
                if ((strcmp(field->c_type, ctx->types[k].name) == 0) && (dependency != file) && (included[dependency] == 0)) { return 0; }
            }
        }
    }

    return 1;
}

static int emit_model_includes(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    int included[GENERATOR_MAX_FILES] = {0};

    for (int pass = 0; pass < ctx->file_count; pass++)
    {
        for (int i = 0; i < ctx->file_count; i++)
        {
            if ((included[i] != 0) || (ctx->files[i].has_table == 0)) { continue; }

            if (model_file_ready(ctx, i, included) != 0)
            {
                (void)fprintf(out, "#include \"%s\"\n", ctx->files[i].path);
                included[i] = 1;
            }
        }
    }

    for (int i = 0; i < ctx->file_count; i++)
    {
        if ((ctx->files[i].has_table != 0) && (included[i] == 0))
        {
            (void)fprintf(stderr, "cyclic model file dependencies: %s\n", ctx->files[i].path);
            return -1;
        }
    }

    return 0;
}

