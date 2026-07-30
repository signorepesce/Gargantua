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

int extract_params(Generator *ctx, ParseState *st, const char *line, ParsedRoute *r, int service)
{
    assert(ctx != NULL);
    assert(r != NULL);

    char inner[GENERATOR_MAX_LINE];
    if (signature_inner(line, inner, sizeof(inner)) != 0) { return -1; }

    char *body = line_trim(inner);
    if ((body[0] == '\0') || (word_equals(body, "void") == 1))
    {
        r->param_count = 0;
        return 0;
    }

    char *save     = body;
    int   finished = 0;

    for (int i = 0; i < GENERATOR_MAX_PARAMS; i++)
    {
        char *comma = strchr(save, ',');
        if (comma != NULL) { *comma = '\0'; }

        if (take_param(ctx, st, r, i, save, service) != 0) { return -1; }

        if (comma == NULL)
        {
            finished = 1;
            break;
        }
        save = comma + 1;
    }

    if (finished == 0)
    {
        generator_error(ctx, st->lineno, "too many parameters (limit %d)", GENERATOR_MAX_PARAMS);
        return -1;
    }

    return 0;
}

static int url_has_param(const char *url, const char *name)
{
    assert(url != NULL);
    assert(name != NULL);

    char needle[GENERATOR_MAX_NAME + 2];
    (void)snprintf(needle, sizeof(needle), "{%s}", name);

    return (strstr(url, needle) != NULL) ? 1 : 0;
}

static int url_param_slot(const char *url, const char *name)
{
    int slot = 0;
    for (size_t i = 0u; i < GENERATOR_MAX_URL && url[i] != '\0'; i++)
    {
        if (url[i] != '{') { continue; }
        size_t start = ++i;
        while (i < GENERATOR_MAX_URL && url[i] != '\0' && url[i] != '}') { i++; }
        if (i >= GENERATOR_MAX_URL || url[i] != '}') { return -1; }
        size_t len = i - start;
        if (strlen(name) == len && memcmp(url + start, name, len) == 0) { return slot; }
        slot++;
    }
    return -1;
}

int count_url_params(const char *url)
{
    assert(url != NULL);

    int n = 0;
    for (int i = 0; (i < GENERATOR_MAX_URL) && (url[i] != '\0'); i++)
    {
        if (url[i] == '{') { n++; }
    }
    return n;
}

int parse_route_url(Generator *ctx, ParseState *st, const char *line, const char *word, char out[GENERATOR_MAX_URL])
{
    const char *open = strchr(line, '(');
    if (open == NULL) { return -1; }

    const char *p = open + 1;
    while ((*p == ' ') || (*p == '\t')) { p++; }
    if (*p != '"')
    {
        generator_error(ctx, st->lineno, "%s requires a quoted path", word);
        return -1;
    }

    const char *start = ++p;
    while ((*p != '\0') && (*p != '"'))
    {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20u) || (c == 0x7fu) || (*p == '\\'))
        {
            generator_error(ctx, st->lineno, "the path contains an unsafe character");
            return -1;
        }
        p++;
    }
    if (*p != '"')
    {
        generator_error(ctx, st->lineno, "the closing quote is missing");
        return -1;
    }

    size_t n = (size_t)(p - start);
    if ((n == 0u) || (n >= GENERATOR_MAX_URL))
    {
        generator_error(ctx, st->lineno, "the path is missing or too long");
        return -1;
    }
    memcpy(out, start, n);
    out[n] = '\0';

    p++;
    while ((*p == ' ') || (*p == '\t')) { p++; }
    if (*p != ')')
    {
        generator_error(ctx, st->lineno, "close %s with ')'", word);
        return -1;
    }
    p++;
    while ((*p == ' ') || (*p == '\t')) { p++; }
    if (*p != '\0')
    {
        generator_error(ctx, st->lineno, "do not put other text after %s(\"...\")", word);
        return -1;
    }
    return 0;
}

typedef struct
{
    char names[GENERATOR_MAX_PARAMS][GENERATOR_MAX_NAME];
    int  count;
} Placeholders;

static int validate_placeholder(Generator *ctx, ParseState *st, const char *url, const char *p, size_t n, Placeholders *seen)
{
    assert(ctx != NULL);
    assert(seen != NULL);

    if ((n < 3u) || (p[n - 1u] != '}') || (memchr(p + 1, '{', n - 1u) != NULL) || (memchr(p + 1, '}', n - 2u) != NULL) || ((n - 2u) >= GENERATOR_MAX_NAME) || (seen->count >= GENERATOR_MAX_PARAMS))
    {
        generator_error(ctx, st->lineno, "invalid placeholder in path '%s'", url);
        return -1;
    }

    char name[GENERATOR_MAX_NAME];
    memcpy(name, p + 1, n - 2u);
    name[n - 2u] = '\0';

    if (is_safe_identifier(name) == 0)
    {
        generator_error(ctx, st->lineno, "unsafe placeholder name '%s'", name);
        return -1;
    }

    for (int i = 0; i < seen->count; i++)
    {
        if (word_equals(seen->names[i], name) == 1)
        {
            generator_error(ctx, st->lineno, "placeholder '%s' appears twice", name);
            return -1;
        }
    }

    (void)snprintf(seen->names[seen->count], GENERATOR_MAX_NAME, "%s", name);
    seen->count++;

    return 0;
}

