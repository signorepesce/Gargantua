#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "generator.h"
#include <assert.h>
#include <ctype.h>
#include <string.h>

void line_strip_comment(char *line)
{
    assert(line != NULL);
    assert(strlen(line) < (size_t)GENERATOR_MAX_LINE);

    char *block = strstr(line, "/*");
    if (block != NULL) { *block = '\0'; }

    char *rest = strstr(line, "//");
    if (rest != NULL) { *rest = '\0'; }
}

char *line_trim(char *line)
{
    assert(line != NULL);
    assert(strlen(line) < (size_t)GENERATOR_MAX_LINE);

    char *start = line;
    int   guard = 0;

    while ((*start != '\0') && (guard < GENERATOR_MAX_LINE))
    {
        if (isspace((unsigned char)*start) == 0) { break; }
        start++;
        guard++;
    }

    size_t len = strlen(start);
    guard = 0;
    while ((len > 0u) && (guard < GENERATOR_MAX_LINE))
    {
        if (isspace((unsigned char)start[len - 1u]) == 0) { break; }
        len--;
        start[len] = '\0';
        guard++;
    }

    return start;
}

int line_split_words(char *line, char *tok[], int max)
{
    assert(line != NULL);
    assert(tok != NULL);
    assert(max > 0);
    assert(max <= GENERATOR_MAX_TOKENS);

    int   n     = 0;
    int   guard = 0;
    char *p     = line;

    while ((*p != '\0') && (n < max) && (guard < GENERATOR_MAX_LINE))
    {
        while ((*p != '\0') && (isspace((unsigned char)*p) != 0) && (guard < GENERATOR_MAX_LINE))
        {
            p++;
            guard++;
        }
        if (*p == '\0') { break; }

        tok[n] = p;
        n++;

        while ((*p != '\0') && (isspace((unsigned char)*p) == 0) && (guard < GENERATOR_MAX_LINE))
        {
            p++;
            guard++;
        }
        if (*p != '\0')
        {
            *p = '\0';
            p++;
        }
    }

    return n;
}

int word_equals(const char *a, const char *b)
{
    assert(a != NULL);
    assert(b != NULL);

    return (strcmp(a, b) == 0) ? 1 : 0;
}

const char *field_kind_from_c_type(const char *c_type)
{
    assert(c_type != NULL);
    if (strlen(c_type) >= GENERATOR_MAX_NAME) { return NULL; }

    if (word_equals(c_type, "int") == 1) { return "FIELD_INT"; }
    if (word_equals(c_type, "long") == 1) { return "FIELD_LONG"; }
    if (word_equals(c_type, "double") == 1) { return "FIELD_DOUBLE"; }
    if (word_equals(c_type, "bool") == 1) { return "FIELD_BOOL"; }
    if (word_equals(c_type, "str") == 1) { return "FIELD_STR"; }
    return NULL;
}

unsigned field_flag_from_word(const char *word)
{
    assert(word != NULL);
    if (strlen(word) >= GENERATOR_MAX_NAME) { return 0; }

    if (word_equals(word, "$id") == 1) { return 1u; }
    if (word_equals(word, "$not_null") == 1) { return 2u; }
    if (word_equals(word, "$unique") == 1) { return 4u; }
    return 0u;
}

const char *path_file_name(const char *path)
{
    assert(path != NULL);
    assert(strlen(path) < (size_t)GENERATOR_MAX_PATH);

    const char *slash = strrchr(path, '/');
    return (slash != NULL) ? (slash + 1) : path;
}

const char *http_method_from_word(const char *word)
{
    assert(word != NULL);
    if (strlen(word) >= GENERATOR_MAX_NAME) { return 0; }

    if (word_equals(word, "$get") == 1)    { return "GET";    }
    if (word_equals(word, "$post") == 1)   { return "POST";   }
    if (word_equals(word, "$put") == 1)    { return "PUT";    }
    if (word_equals(word, "$patch") == 1)  { return "PATCH";  }
    if (word_equals(word, "$delete") == 1) { return "DELETE"; }
    return NULL;
}

int line_quoted_text(const char *line, char *out, size_t cap)
{
    assert(line != NULL);
    assert(out != NULL);
    assert(cap > 1u);

    const char *open = strchr(line, '"');
    if (open == NULL) { return -1; }
    const char *close = strchr(open + 1, '"');
    if (close == NULL) { return -1; }

    size_t n = (size_t)(close - open) - 1u;
    if ((n == 0u) || (n >= cap)) { return -1; }

    memcpy(out, open + 1, n);
    out[n] = '\0';
    return 0;
}
