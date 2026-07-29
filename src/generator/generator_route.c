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

static const char *signature_open_paren(const char *line)
{
    const char *start = line;
    if (strncmp(line, "$list(", 6u) == 0 || strncmp(line, "$page(", 6u) == 0)
    {
        const char *close = strchr(line, ')');
        if (close == NULL) { return NULL; }
        start = close + 1;
    }
    return strchr(start, '(');
}

int extract_function_name(const char *line, char *out, size_t cap)
{
    assert(line != NULL);
    assert(out != NULL);
    assert(cap > 1u);

    const char *paren = signature_open_paren(line);
    if (paren == NULL) { return -1; }

    const char *end = paren;
    int guard = 0;
    while ((end > line) && (guard < GENERATOR_MAX_LINE))
    {
        if (isspace((unsigned char)*(end - 1)) == 0) { break; }
        end--;
        guard++;
    }

    const char *start = end;
    guard = 0;
    while ((start > line) && (guard < GENERATOR_MAX_LINE))
    {
        char c = *(start - 1);
        if ((isalnum((unsigned char)c) == 0) && (c != '_')) { break; }
        start--;
        guard++;
    }

    size_t n = (size_t)(end - start);
    if ((n == 0u) || (n >= cap)) { return -1; }

    memcpy(out, start, n);
    out[n] = '\0';
    return (is_safe_identifier(out) != 0) ? 0 : -1;
}

int extract_return_type(const char *line, char *out, size_t cap)
{
    assert(line != NULL);
    assert(out != NULL);
    assert(cap > 1u);

    const char *paren = signature_open_paren(line);
    if (paren == NULL) { return -1; }

    const char *start = paren;
    int guard = 0;
    while ((start > line) && (guard < GENERATOR_MAX_LINE))
    {
        char c = *(start - 1);
        if ((isalnum((unsigned char)c) == 0) && (c != '_')) { break; }
        start--;
        guard++;
    }

    char work[GENERATOR_MAX_LINE];
    size_t n = (size_t)(start - line);
    if (n >= sizeof(work)) { return -1; }
    memcpy(work, line, n);
    work[n] = '\0';

    char *t = line_trim(work);
    if ((t[0] == '\0') || (strlen(t) >= cap)) { return -1; }
    if (strchr(t, '*') != NULL) { return -1; }

    (void)snprintf(out, cap, "%s", t);
    return 0;
}

static int signature_inner(const char *line, char *inner, size_t cap)
{
    assert(line != NULL);
    assert(inner != NULL);

    const char *open  = signature_open_paren(line);
    const char *close = (open != NULL) ? strrchr(line, ')') : NULL;
    if ((open == NULL) || (close == NULL) || (close < open)) { return -1; }

    size_t n = (size_t)(close - open) - 1u;
    if (n >= cap) { return -1; }

    memcpy(inner, open + 1, n);
    inner[n] = '\0';

    return 0;
}

static int param_type_allowed(const char *type, int service)
{
    assert(type != NULL);

    return ((word_equals(type, "int") == 1) || (word_equals(type, "str") == 1) || (service && ((word_equals(type, "long") == 1) || (word_equals(type, "double") == 1) || (word_equals(type, "bool") == 1))) || (is_safe_identifier(type) != 0)) ? 1 : 0;
}

static int take_param(Generator *ctx, ParseState *st, ParsedRoute *r, int slot, char *text, int service)
{
    assert(ctx != NULL);
    assert(r != NULL);

    char *one = line_trim(text);
    char *tok[GENERATOR_MAX_TOKENS];

    if (line_split_words(one, tok, GENERATOR_MAX_TOKENS) != 2)
    {
        generator_error(ctx, st->lineno, "the parameter must be 'type name'");
        return -1;
    }

    if ((param_type_allowed(tok[0], service) == 0) || (is_safe_identifier(tok[1]) == 0))
    {
        generator_error(ctx, st->lineno, "parameter type and name must be safe identifiers");
        return -1;
    }

    for (int prior = 0; prior < slot; prior++)
    {
        if (word_equals(r->params[prior].name, tok[1]) == 1)
        {
            generator_error(ctx, st->lineno, "parameter '%s' is declared twice", tok[1]);
            return -1;
        }
    }

    int scalar = ((word_equals(tok[0], "int") == 1) || (word_equals(tok[0], "str") == 1)) ? 1 : 0;

    (void)snprintf(r->params[slot].type, GENERATOR_MAX_NAME, "%s", tok[0]);
    (void)snprintf(r->params[slot].name, GENERATOR_MAX_NAME, "%s", tok[1]);
    r->params[slot].is_body = (scalar == 1) ? 0 : 1;
    r->param_count = slot + 1;

    return 0;
}

