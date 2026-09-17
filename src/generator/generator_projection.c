#define _POSIX_C_SOURCE 200809L
#include "parse_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int parse_pick(Generator *ctx, ParseState *st, char *line)
{
    if (strncmp(line, "$pick(", 6u) != 0)
    {
        return 0;
    }
    ParsedType *type = st->current_type;
    size_t length = strlen(line);
    if (type == NULL || !type->json_only || type->field_count || st->pending || st->rules.validation ||
        st->pending_references[0] || length < 9u || line[length - 1u] != ')')
    {
        generator_error(ctx, st->lineno, "write one $pick(Table, field, ...) first inside $json");
        return 1;
    }
    line[length - 1u] = '\0';
    char *cursor = line + 6;
    for (int slot = 0; slot <= GENERATOR_MAX_FIELDS; slot++)
    {
        char *comma = strchr(cursor, ',');
        if (comma != NULL)
        {
            *comma = '\0';
        }
        char *name = line_trim(cursor);
        if (strlen(name) >= GENERATOR_MAX_NAME || !is_safe_identifier(name))
        {
            generator_error(ctx, st->lineno, "$pick requires plain table and field names");
            return 1;
        }
        if (!slot)
        {
            (void)snprintf(type->source_type, sizeof(type->source_type), "%s", name);
        }
        else
        {
            for (int i = 0; i < type->field_count; i++)
            {
                if (strcmp(type->fields[i].name, name) == 0)
                {
                    generator_error(ctx, st->lineno, "$pick repeats field '%s'", name);
                    return 1;
                }
            }
            ParsedField *field = &type->fields[type->field_count++];
            (void)snprintf(field->name, sizeof(field->name), "%s", name);
            type->pick_count++;
        }
        if (comma == NULL)
        {
            if (!type->pick_count)
            {
                generator_error(ctx, st->lineno, "$pick needs at least one field");
            }
            return 1;
        }
        cursor = comma + 1;
    }
    generator_error(ctx, st->lineno, "$pick exceeds the field limit");
    return 1;
}

void resolve_picks(Generator *ctx)
{
    for (int i = 0; i < ctx->type_count; i++)
    {
        ParsedType *target = &ctx->types[i];
        if (!target->pick_count)
        {
            continue;
        }
        generator_select_file(ctx, target->file_index);
        const ParsedType *source = NULL;
        for (int j = 0; j < ctx->type_count; j++)
        {
            if (strcmp(ctx->types[j].name, target->source_type) == 0)
            {
                source = &ctx->types[j];
                break;
            }
        }
        if (source == NULL || source->json_only)
        {
            generator_error(ctx, target->line, "$pick source '%s' must be a $table", target->source_type);
            continue;
        }
        if (source->file_index == target->file_index && source >= target)
        {
            generator_error(ctx, target->line, "declare the source table before its DTO in the same file");
        }
        for (int j = 0; j < target->pick_count; j++)
        {
            ParsedField *field = &target->fields[j];
            int found = 0;
            for (int k = 0; k < source->field_count; k++)
            {
                if (strcmp(source->fields[k].name, field->name) != 0)
                {
                    continue;
                }
                *field = source->fields[k];
                field->flags &= 10u;
                field->references[0] = '\0';
                field->reference_key[0] = '\0';
                found = 1;
                break;
            }
            if (!found)
            {
                generator_error(ctx, target->line, "$pick: no field '%s' in %s", field->name, source->name);
            }
        }
    }
}

static int emit_model_line(FILE *out, const SourceFile *file, const char *line)
{
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t')
    {
        cursor++;
    }
    if (*cursor == '#')
    {
        cursor++;
        while (*cursor == ' ' || *cursor == '\t')
        {
            cursor++;
        }
        if (strncmp(cursor, "include", 7u) == 0)
        {
            cursor += 7;
            while (*cursor == ' ' || *cursor == '\t')
            {
                cursor++;
            }
            const char *end = *cursor == '"' ? strchr(cursor + 1, '"') : NULL;
            const char *slash = strrchr(file->path, '/');
            if (end != NULL && slash != NULL && cursor[1] != '/')
            {
                char local[GENERATOR_MAX_PATH];
                int n = snprintf(local, sizeof(local), "%.*s/%.*s", (int)(slash - file->path), file->path,
                                 (int)(end - cursor - 1), cursor + 1);
                if (n < 0 || (size_t)n >= sizeof(local))
                {
                    return -1;
                }
                if (access(local, R_OK) == 0)
                {
                    return fprintf(out, "#include \"%s\"\n", local) < 0 ? -1 : 0;
                }
            }
        }
    }
    return fprintf(out, "%s\n", line) < 0 ? -1 : 0;
}

int emit_model_file(FILE *out, const Generator *ctx, int file_index)
{
    int has_pick = 0;
    for (int i = 0; i < ctx->type_count; i++)
    {
        if (ctx->types[i].file_index == file_index && ctx->types[i].pick_count)
        {
            has_pick = 1;
        }
    }
    if (!has_pick)
    {
        return fprintf(out, "#include \"%s\"\n", ctx->files[file_index].path) < 0 ? -1 : 0;
    }
    FILE *input = fopen(ctx->files[file_index].path, "r");
    if (input == NULL)
    {
        return -1;
    }
    char line[GENERATOR_MAX_LINE + 2];
    const ParsedType *type = NULL;
    int block = 0;
    int number = 0;
    int result = 0;
    while (fgets(line, (int)sizeof(line), input) != NULL)
    {
        number++;
        strip_comments(line, &block);
        char *trimmed = line_trim(line);
        for (int i = 0; i < ctx->type_count; i++)
        {
            if (ctx->types[i].file_index == file_index && ctx->types[i].line == number)
            {
                type = &ctx->types[i];
            }
        }
        (void)fprintf(out, "#line %d \"%s\"\n", number, ctx->files[file_index].path);
        if (strncmp(trimmed, "$pick(", 6u) == 0 && type != NULL)
        {
            for (int f = 0; f < type->pick_count; f++)
            {
                const ParsedField *field = &type->fields[f];
                (void)fprintf(out, "    %s%s%s %s;\n", (field->flags & 8u) ? "$nullable(" : "", field->c_type,
                              (field->flags & 8u) ? ")" : "", field->name);
            }
        }
        else if (emit_model_line(out, &ctx->files[file_index], line) != 0)
        {
            result = -1;
            break;
        }
    }
    if (ferror(input))
    {
        result = -1;
    }
    if (fclose(input) != 0)
    {
        result = -1;
    }
    return result;
}

void emit_projection(FILE *out, const ParsedType *type)
{
    if (!type->pick_count)
    {
        return;
    }
    (void)fprintf(out, "\nstatic inline %s $%s_from(%s wrap_source)\n{\n    %s wrap_result = {0};\n", type->name,
                  type->name, type->source_type, type->name);
    for (int i = 0; i < type->pick_count; i++)
    {
        const char *field = type->fields[i].name;
        (void)fprintf(out, "    wrap_result.%s = wrap_source.%s;\n", field, field);
    }
    (void)fprintf(out, "    return wrap_result;\n}\n");
}
