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

static int is_c_keyword(const char *name)
{
    static const char *const words[] = {
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex",
        "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
        "_Thread_local", "auto", "break", "case", "char", "const",
        "continue", "default", "do", "double", "else", "enum", "extern",
        "float", "for", "goto", "if", "inline", "int", "long",
        "register", "restrict", "return", "short", "signed", "sizeof",
        "static", "struct", "switch", "typedef", "union", "unsigned",
        "void", "volatile", "while"
    };

    for (size_t i = 0u; i < (sizeof(words) / sizeof(words[0])); i++)
    {
        if (strcmp(name, words[i]) == 0) { return 1; }
    }
    return 0;
}

int is_safe_identifier(const char *name)
{
    assert(name != NULL);

    unsigned char first = (unsigned char)name[0];
    if ((isalpha(first) == 0) || (strncmp(name, "wrap_", 5u) == 0) || (is_c_keyword(name) != 0)) { return 0; }

    for (size_t i = 1u; i < GENERATOR_MAX_NAME; i++)
    {
        if (name[i] == '\0') { return 1; }
        unsigned char c = (unsigned char)name[i];
        if ((isalnum(c) == 0) && (c != (unsigned char)'_')) { return 0; }
    }
    return 0;
}

void strip_comments(char *line, int *in_block)
{
    assert(line != NULL);
    assert(in_block != NULL);

    size_t read = 0u;
    size_t write = 0u;
    char quote = '\0';

    while ((read < GENERATOR_MAX_LINE) && (line[read] != '\0'))
    {
        if (*in_block != 0)
        {
            if ((line[read] == '*') && (line[read + 1u] == '/'))
            {
                *in_block = 0;
                read += 2u;
            }
            else { read++; }
            continue;
        }

        if (quote != '\0')
        {
            char c = line[read++];
            line[write++] = c;
            if ((c == '\\') && (line[read] != '\0'))
            {
                line[write++] = line[read++];
            }
            else if (c == quote) { quote = '\0'; }
            continue;
        }

        if ((line[read] == '/') && (line[read + 1u] == '/')) { break; }
        if ((line[read] == '/') && (line[read + 1u] == '*'))
        {
            *in_block = 1;
            read += 2u;
            continue;
        }
        if ((line[read] == '\'') || (line[read] == '"')) { quote = line[read]; }
        line[write++] = line[read++];
    }
    line[write] = '\0';
}

int parse_table_name(Generator *ctx, int line_no, const char *line, char *out, size_t cap)
{
    assert(ctx != NULL);
    assert(line != NULL);
    assert(out != NULL);
    assert(cap > 1u);

    if ((strncmp(line, "$table(", 7u) != 0) && (strncmp(line, "$json(", 5u) != 0))
    {
        generator_error(ctx, line_no, "write $table(Name)");
        return -1;
    }

    const char *open  = strchr(line, '(');
    const char *close = strchr(open + 1, ')');
    if ((close == NULL) || (close[1] != '\0'))
    {
        generator_error(ctx, line_no, "write $table(Name), with no other text");
        return -1;
    }

    size_t n = (size_t)(close - open) - 1u;
    if ((n == 0u) || (n >= cap))
    {
        generator_error(ctx, line_no, "the table name is missing or too long");
        return -1;
    }

    memcpy(out, open + 1, n);
    out[n] = '\0';
    char *trimmed = line_trim(out);
    if ((trimmed != out) && (trimmed[0] != '\0'))
    {
        size_t trimmed_n = strlen(trimmed);
        memmove(out, trimmed, trimmed_n + 1u);
    }
    if ((out[0] == '\0') || (is_safe_identifier(out) == 0))
    {
        generator_error(ctx, line_no, "table name '%s' is not a valid C identifier", out);
        return -1;
    }
    return 0;
}

void generator_init(Generator *ctx)
{
    assert(ctx != NULL);
    assert(sizeof(*ctx) > 0u);

    memset(ctx, 0, sizeof(*ctx));
}

int generator_begin_file(Generator *ctx, const char *path)
{
    assert(ctx != NULL);
    assert(path != NULL);

    if (ctx->file_count >= GENERATOR_MAX_FILES)
    {
        (void)fprintf(stderr, "gargantua: too many files (limit %d)\n", GENERATOR_MAX_FILES);
        return -1;
    }
    if ((strlen(path) >= (size_t)GENERATOR_MAX_PATH) || (strpbrk(path, "\"\r\n") != NULL))
    {
        (void)fprintf(stderr, "gargantua: unsafe or too long path\n");
        return -1;
    }

    (void)snprintf(ctx->path, sizeof(ctx->path), "%s", path);
    (void)snprintf(ctx->files[ctx->file_count].path, GENERATOR_MAX_PATH, "%s", path);
    ctx->files[ctx->file_count].has_table = 0;
    ctx->files[ctx->file_count].has_route = 0;
    ctx->files[ctx->file_count].has_service = 0;
    ctx->file_count++;
    return 0;
}

void generator_error(Generator *ctx, int line, const char *fmt, ...)
{
    assert(ctx != NULL);
    assert(fmt != NULL);

    va_list ap;
    (void)fprintf(stderr, "%s:%d: ", ctx->path, line);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fputc('\n', stderr);

    ctx->errors++;
}

int parse_field(Generator *ctx, char *line, int lineno, ParsedField *out)
{
    assert(ctx != NULL);
    assert(line != NULL);
    assert(out != NULL);

    char *tok[GENERATOR_MAX_TOKENS];
    int   n = line_split_words(line, tok, GENERATOR_MAX_TOKENS);
    if (n == 0) { return 0; }

    memset(out, 0, sizeof(*out));

    int i = 0;
    while ((i < n) && (i < GENERATOR_MAX_TOKENS))
    {
        unsigned flag = field_flag_from_word(tok[i]);
        if (flag == 0u) { break; }
        out->flags |= flag;
        i++;
    }

    if ((n - i) != 2)
    {
        generator_error(ctx, lineno, "could not parse the field. Expected 'type name;'");
        return -1;
    }

    if (strlen(tok[i]) >= GENERATOR_MAX_NAME || strlen(tok[i + 1]) >= GENERATOR_MAX_NAME)
    {
        generator_error(ctx, lineno, "field type or name is too long");
        return -1;
    }
    const char *kind = field_kind_from_c_type(tok[i]);
    if (kind == NULL)
    {
        if (is_safe_identifier(tok[i]) == 0)
        {
            generator_error(ctx, lineno, "invalid field type '%s'", tok[i]);
            return -1;
        }
        kind = "FIELD_OBJECT";
    }
    if (is_safe_identifier(tok[i + 1]) == 0)
    {
        generator_error(ctx, lineno, "field '%s' must be a plain, safe C identifier", tok[i + 1]);
        return -1;
    }

    (void)snprintf(out->c_type, sizeof(out->c_type), "%s", tok[i]);
    (void)snprintf(out->kind, sizeof(out->kind), "%s", kind);
    (void)snprintf(out->name, sizeof(out->name), "%s", tok[i + 1]);
    return 1;
}

int line_only_annotations(const char *line, unsigned *flags)
{
    assert(line != NULL);
    assert(flags != NULL);

    char copy[GENERATOR_MAX_LINE];
    (void)snprintf(copy, sizeof(copy), "%s", line);

    char *tok[GENERATOR_MAX_TOKENS];
    int   n = line_split_words(copy, tok, GENERATOR_MAX_TOKENS);
    if (n == 0) { return 0; }

    unsigned acc = 0u;
    for (int i = 0; (i < n) && (i < GENERATOR_MAX_TOKENS); i++)
    {
        unsigned flag = field_flag_from_word(tok[i]);
        if (flag == 0u) { return 0; }
        acc |= flag;
    }

    *flags |= acc;
    return 1;
}
