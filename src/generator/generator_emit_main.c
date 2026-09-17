#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "generator.h"
#include "parse_internal.h"
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "emit_internal.h"

static void emit_service_params(FILE *out, const ParsedService *service, int names)
{
    assert(out != NULL);
    assert(service != NULL);

    if (service->param_count == 0)
    {
        if (!names)
        {
            (void)fprintf(out, "void");
        }
        return;
    }
    for (int i = 0; i < service->param_count && i < GENERATOR_MAX_PARAMS; i++)
    {
        const ParsedParam *param = &service->params[i];
        (void)fprintf(out, "%s%s%s", i ? ", " : "", names ? "" : param->type, names ? param->name : "");
        if (!names)
        {
            (void)fprintf(out, " %s", param->name);
        }
    }
}

static const char *service_name_in_line(const char *line, const char *name)
{
    assert(line != NULL);
    assert(name != NULL);

    size_t length = strlen(name);
    int block = 0;
    char quote = '\0';
    for (size_t i = 0u; line[i] != '\0' && i < GENERATOR_MAX_LINE; i++)
    {
        char c = line[i];
        if (block)
        {
            if (c == '*' && line[i + 1u] == '/')
            {
                block = 0;
                i++;
            }
            continue;
        }
        if (quote)
        {
            if (c == '\\' && line[i + 1u])
            {
                i++;
            }
            else if (c == quote)
            {
                quote = '\0';
            }
            continue;
        }
        if (c == '/' && line[i + 1u] == '*')
        {
            block = 1;
            i++;
            continue;
        }
        if (c == '/' && line[i + 1u] == '/')
        {
            break;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (i && (isalnum((unsigned char)line[i - 1u]) || line[i - 1u] == '_'))
        {
            continue;
        }
        if (strncmp(line + i, name, length) != 0)
        {
            continue;
        }
        const char *after = line + i + length;
        while (*after == ' ' || *after == '\t')
        {
            after++;
        }
        if (*after == '(')
        {
            return line + i;
        }
    }
    return NULL;
}

static int emit_service_input(FILE *out, const Generator *ctx, int file_index)
{
    assert(out != NULL);
    assert(ctx != NULL);
    assert(file_index >= 0 && file_index < ctx->file_count);

    FILE *source = fopen(ctx->files[file_index].path, "r");
    if (source == NULL)
    {
        return -1;
    }
    (void)fprintf(out, "#line 1 \"%s\"\n", ctx->files[file_index].path);
    char line[GENERATOR_MAX_LINE + 2];
    int line_number = 0;
    int in_block = 0;
    int transformed[GENERATOR_MAX_SERVICES] = {0};
    int result = 0;
    while (fgets(line, (int)sizeof(line), source) != NULL)
    {
        line_number++;
        strip_comments(line, &in_block);
        size_t length = strlen(line);
        if (length == 0u || line[length - 1u] != '\n')
        {
            line[length++] = '\n';
            line[length] = '\0';
        }
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
            const char *inc = line;
            while (*inc == ' ' || *inc == '\t')
            {
                inc++;
            }
            if (*inc == '#')
            {
                inc++;
                while (*inc == ' ' || *inc == '\t')
                {
                    inc++;
                }
                if (strncmp(inc, "include", 7u) == 0)
                {
                    inc += 7;
                    while (*inc == ' ' || *inc == '\t')
                    {
                        inc++;
                    }
                    const char *end = *inc == '"' ? strchr(inc + 1, '"') : NULL;
                    const char *slash = strrchr(ctx->files[file_index].path, '/');
                    if (end != NULL && slash != NULL && inc[1] != '/')
                    {
                        char local[GENERATOR_MAX_PATH];
                        int n = snprintf(local, sizeof(local), "%.*s/%.*s", (int)(slash - ctx->files[file_index].path),
                                         ctx->files[file_index].path, (int)(end - inc - 1), inc + 1);
                        if (n < 0 || (size_t)n >= sizeof(local))
                        {
                            result = -1;
                            break;
                        }
                        if (access(local, R_OK) == 0)
                        {
                            (void)fprintf(out, "#include \"%s\"\n", local);
                            continue;
                        }
                    }
                }
            }
            if (fputs(line, out) == EOF)
            {
                result = -1;
                break;
            }
            continue;
        }
        const char *name = service_name_in_line(line, match->function_name);
        if (name == NULL)
        {
            result = -1;
            break;
        }
        size_t prefix = (size_t)(name - line);
        if (fputs("static ", out) == EOF || fwrite(line, 1u, prefix, out) != prefix ||
            fprintf(out, "wrap_service_%s%s", match->function_name, name + strlen(match->function_name)) < 0)
        {
            result = -1;
            break;
        }
        transformed[match_index] = 1;
    }
    if (ferror(source) != 0)
    {
        result = -1;
    }
    if (fclose(source) != 0)
    {
        result = -1;
    }
    for (int i = 0; i < ctx->service_count && i < GENERATOR_MAX_SERVICES; i++)
    {
        if (ctx->services[i].file_index == file_index && !transformed[i])
        {
            result = -1;
        }
    }
    return result;
}

static void emit_service_wrapper(FILE *out, const ParsedService *service)
{
    assert(out != NULL);
    assert(service != NULL);

    (void)fprintf(out, "\n%s %s(", service->return_type, service->function_name);
    emit_service_params(out, service, 0);
    (void)fprintf(out,
                  ")\n{\n"
                  "    Transaction wrap_scope __attribute__((cleanup(transaction_cleanup))) = transaction_begin();\n");
    if (strcmp(service->return_type, "void") == 0)
    {
        (void)fprintf(out,
                      "    if (!wrap_scope.active)\n"
                      "    {\n"
                      "        return;\n"
                      "    }\n"
                      "    wrap_service_%s(",
                      service->function_name);
        emit_service_params(out, service, 1);
        (void)fprintf(out, ");\n"
                           "    transaction_end(&wrap_scope);\n"
                           "}\n");
        return;
    }
    (void)fprintf(out,
                  "    %s wrap_value = {0};\n"
                  "    if (!wrap_scope.active)\n"
                  "    {\n"
                  "        return wrap_value;\n"
                  "    }\n"
                  "    wrap_value = wrap_service_%s(",
                  service->return_type, service->function_name);
    emit_service_params(out, service, 1);
    (void)fprintf(out, ");\n"
                       "    transaction_end(&wrap_scope);\n"
                       "    return wrap_value;\n"
                       "}\n");
}

static int model_file_ready(const Generator *ctx, int file, const int *included)
{
    assert(ctx != NULL);
    assert(included != NULL);

    for (int t = 0; t < ctx->type_count; t++)
    {
        if (ctx->types[t].file_index != file)
        {
            continue;
        }

        for (int k = 0; k < ctx->type_count; k++)
        {
            int dependency = ctx->types[k].file_index;
            if (strcmp(ctx->types[t].source_type, ctx->types[k].name) == 0 && dependency != file &&
                !included[dependency])
            {
                return 0;
            }
        }

        for (int f = 0; f < ctx->types[t].field_count; f++)
        {
            const ParsedField *field = &ctx->types[t].fields[f];
            if (strcmp(field->kind, "FIELD_OBJECT") != 0)
            {
                continue;
            }

            for (int k = 0; k < ctx->type_count; k++)
            {
                int dependency = ctx->types[k].file_index;
                if ((strcmp(field->c_type, ctx->types[k].name) == 0) && (dependency != file) &&
                    (included[dependency] == 0))
                {
                    return 0;
                }
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
            if ((included[i] != 0) || (ctx->files[i].has_table == 0))
            {
                continue;
            }

            if (model_file_ready(ctx, i, included) != 0)
            {
                if (emit_model_file(out, ctx, i) != 0)
                {
                    return -1;
                }
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

static void emit_declarations(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    if (ctx->type_count > 0)
    {
        (void)fprintf(out, "\n");
        for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
        {
            (void)fprintf(out, "extern const TypeInfo %s__type;\n", ctx->types[i].name);
        }

        (void)fprintf(out, "\n");
        for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
        {
            emit_crud_declarations(out, &ctx->types[i]);
        }
    }

    for (int i = 0; i < ctx->type_count; i++)
    {
        emit_projection(out, &ctx->types[i]);
    }
    emit_store_declarations(out, ctx);

    for (int i = 0; i < ctx->function_count; i++)
    {
        const ParsedFunction *f = &ctx->functions[i];
        (void)fprintf(out, "#line %d \"%s\"\n%s;\n", f->line, ctx->files[f->file_index].path, f->signature);
    }
}

static int emit_units(const Generator *ctx, const char *out_path)
{
    char header[GENERATOR_MAX_PATH];
    int n = snprintf(header, sizeof(header), "%s.api.h", out_path);
    if (n < 0 || (size_t)n >= sizeof(header))
    {
        return -1;
    }
    FILE *out = fopen(header, "w");
    if (out == NULL)
    {
        return -1;
    }
    (void)fprintf(out, "#ifndef GARGANTUA_APP_API_H\n#define GARGANTUA_APP_API_H\n"
                       "#include \"framework_internal.h\"\n#include \"db.h\"\n#include \"store.h\"\n");
    int result = emit_model_includes(out, ctx);
    emit_declarations(out, ctx);
    (void)fprintf(out, "\n#endif\n");
    if (ferror(out))
    {
        result = -1;
    }
    if (fclose(out) != 0 || result != 0)
    {
        return -1;
    }
    for (int i = 0; i < ctx->file_count; i++)
    {
        if (ctx->files[i].has_table)
        {
            continue;
        }
        char unit[GENERATOR_MAX_PATH];
        n = snprintf(unit, sizeof(unit), "%s.unit-%d.c", out_path, i);
        if (n < 0 || (size_t)n >= sizeof(unit))
        {
            return -1;
        }
        out = fopen(unit, "w");
        if (out == NULL)
        {
            return -1;
        }
        (void)fprintf(out, "#include \"%s\"\n", path_file_name(header));
        if (ctx->files[i].has_service)
        {
            result = emit_service_input(out, ctx, i);
            (void)fprintf(out, "\n#line 1 \"gargantua-generated-wrappers\"\n");
            for (int j = 0; j < ctx->service_count; j++)
            {
                if (ctx->services[j].file_index == i)
                {
                    emit_service_wrapper(out, &ctx->services[j]);
                }
            }
        }
        else
        {
            (void)fprintf(out, "#include \"%s\"\n", ctx->files[i].path);
        }
        if (fclose(out) != 0 || result != 0)
        {
            return -1;
        }
    }
    return 0;
}

static void emit_type_tables(FILE *out, const Generator *ctx)
{
    assert(out != NULL);
    assert(ctx != NULL);

    for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
    {
        const ParsedType *type = &ctx->types[i];
        emit_fields(out, type);
        emit_type(out, type);
        emit_crud(out, type);
    }
}

static void emit_main(FILE *out, const Generator *ctx)
{
    assert(out != NULL);

    (void)fprintf(out, "\nstatic int wrap_validate_schema(void)\n{\n");
    for (int i = 0; i < ctx->type_count; i++)
    {
        if (!ctx->types[i].json_only)
        {
            (void)fprintf(out, "    if (db_check_schema(&%s__type) != 0) { return -1; }\n", ctx->types[i].name);
        }
    }
    (void)fprintf(out, "    return 0;\n}\n");
    (void)fprintf(out, "\n\n"
                       "#include \"config.h\"\n"
                       "#include \"migrate.h\"\n"
                       "#include \"db.h\"\n"
                       "#include \"server.h\"\n"
                       "#include \"scheduler.h\"\n"
                       "#include \"fetch.h\"\n"
                       "#include \"template.h\"\n"
                       "#include <stdio.h>\n"
                       "#include <string.h>\n\n"
                       "int main(void)\n"
                       "{\n"
                       "    if (config_load(CONFIG_FILE) < 0)\n"
                       "    {\n"
                       "        return 1;\n"
                       "    }\n\n"
                       "#ifdef GARGANTUA_FEATURE_FETCH\n"
                       "    if (fetch_init() != 0) { return 1; }\n"
                       "#endif\n"
                       "#ifdef GARGANTUA_FEATURE_TEMPLATES\n"
                       "    if (template_init() != 0) { return 1; }\n"
                       "#endif\n"
                       "    if (auth_init() != 0 || db_limits(config_int(\"database.busy_ms\", 1000),\n"
                       "        config_int(\"database.query_ms\", 5000)) != 0) { return 1; }\n"
                       "    str configured_db = config_str(\"database.driver\","
                       " db_name());\n"
                       "    if (strcmp(configured_db, db_name()) != 0)\n"
                       "    {\n"
                       "        (void)fprintf(stderr,"
                       " \"database.driver=%%s but driver %%s was built\\n\","
                       " configured_db, db_name());\n"
                       "        return 1;\n"
                       "    }\n\n"
                       "    {\n"
                       "        str url = config_str(\"database.url\", \":memory:\");\n"
                       "        if ((url[0] == '\\0') || (db_open(url) != 0))\n"
                       "        {\n"
                       "            (void)fprintf(stderr,"
                       " \"database.url is missing or the connection failed\\n\");\n"
                       "            return 1;\n"
                       "        }\n"
                       "        (void)printf(\"database: %%s (connected)\\n\","
                       " db_name());\n"
                       "    }\n\n"
                       "    if (migrate_run(config_str(\"database.migrations\", \"\")) != 0)\n"
                       "    {\n        db_close();\n        return 1;\n    }\n"
                       "    int port = config_int(\"server.port\", 8100);\n"
                       "    if ((port <= 0) || (port > 65535))\n"
                       "    {\n"
                       "        port = 8100;\n"
                       "    }\n\n"
                       "#ifdef GARGANTUA_FEATURE_SCHEDULER\n"
                       "    if (scheduler_startup() != 0)\n"
                       "    {\n        db_close();\n        return 1;\n    }\n"
                       "#endif\n\n"
                       "    if (wrap_validate_schema() != 0)\n"
                       "    {\n#ifdef GARGANTUA_FEATURE_SCHEDULER\n        scheduler_stop();\n#endif\n"
                       "        db_close();\n        return 1;\n    }\n"
                       "#ifdef GARGANTUA_FEATURE_SCHEDULER\n"
                       "    if (scheduler_start() != 0) { db_close(); return 1; }\n#endif\n"
                       "    int rc = server_run(port);\n\n"
                       "#ifdef GARGANTUA_FEATURE_SCHEDULER\n"
                       "    scheduler_stop();\n"
                       "#endif\n"
                       "#ifdef GARGANTUA_FEATURE_FETCH\n"
                       "    fetch_shutdown();\n"
                       "#endif\n"
                       "    db_close();\n"
                       "    return (rc == 0) ? 0 : 1;\n"
                       "}\n");
}

int generator_write_source(const Generator *ctx, const char *out_path)
{
    assert(ctx != NULL);
    assert(out_path != NULL);

    FILE *out = fopen(out_path, "w");
    if (out == NULL)
    {
        (void)fprintf(stderr, "gargantua: could not write %s\n", out_path);
        return -1;
    }

    (void)fprintf(out, ""
                       "#include \"framework_internal.h\"\n"
                       "#include \"db.h\"\n"
                       "#include \"route.h\"\n"
                       "#include \"scheduler.h\"\n"
                       "#include \"fetch.h\"\n"
                       "#include \"template.h\"\n"
                       "#include \"store.h\"\n"
                       "#include <string.h>\n"
                       "#include <limits.h>\n#include <math.h>\n#include <stdio.h>\n\n");

    if (emit_units(ctx, out_path) != 0)
    {
        (void)fclose(out);
        return -1;
    }
    (void)fprintf(out, "#include \"%s.api.h\"\n#line 1 \"gargantua-generated-runtime\"\n", path_file_name(out_path));

    emit_type_tables(out, ctx);
    emit_stores(out, ctx);

    if (ctx->route_count > 0)
    {
        emit_route_wrappers(out, ctx);
    }
    if (generator_write_openapi(out, ctx) != 0)
    {
        (void)fclose(out);
        return -1;
    }

    emit_route_table(out, ctx);
    emit_task_table(out, ctx);

    if (ctx->want_main == 1)
    {
        emit_main(out, ctx);
    }

    if (fclose(out) != 0)
    {
        (void)fprintf(stderr, "gargantua: could not close %s\n", out_path);
        return -1;
    }

    return 0;
}

int generator_write_header(const Generator *ctx, const char *out_path)
{
    assert(ctx != NULL);
    assert(out_path != NULL);

    FILE *out = fopen(out_path, "w");
    if (out == NULL)
    {
        (void)fprintf(stderr, "gargantua: could not write %s\n", out_path);
        return -1;
    }

    (void)fprintf(out, ""
                       "#ifndef GARGANTUA_TYPES_H\n"
                       "#define GARGANTUA_TYPES_H\n\n"
                       "#include \"framework_internal.h\"\n\n");

    for (int i = 0; (i < ctx->type_count) && (i < GENERATOR_MAX_TYPES); i++)
    {
        (void)fprintf(out,
                      "typedef struct %s %s;\n"
                      "extern const TypeInfo %s__type;\n",
                      ctx->types[i].name, ctx->types[i].name, ctx->types[i].name);
        emit_crud_declarations(out, &ctx->types[i]);
    }

    (void)fprintf(out, "\n#endif\n");

    if (fclose(out) != 0)
    {
        (void)fprintf(stderr, "gargantua: could not close %s\n", out_path);
        return -1;
    }
    return 0;
}
