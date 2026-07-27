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

