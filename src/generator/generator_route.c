#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
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

static int validate_route_segment(Generator *ctx, ParseState *st, const char *url, const char *p, size_t n, Placeholders *seen)
{
    assert(ctx != NULL);

    if (n == 0u)
    {
        generator_error(ctx, st->lineno, "path '%s' has an empty segment", url);
        return -1;
    }

    if (((n == 1u) && (p[0] == '.')) || ((n == 2u) && (p[0] == '.') && (p[1] == '.')))
    {
        generator_error(ctx, st->lineno, "path '%s' contains a dot segment", url);
        return -1;
    }

    if (p[0] == '{') { return validate_placeholder(ctx, st, url, p, n, seen); }

    if ((memchr(p, '{', n) != NULL) || (memchr(p, '}', n) != NULL))
    {
        generator_error(ctx, st->lineno, "'{' and '}' must cover a whole segment");
        return -1;
    }

    return 0;
}

static int route_url_shape_valid(Generator *ctx, ParseState *st, const char *url)
{
    assert(ctx != NULL);

    if ((url[0] != '/') || (strchr(url, '?') != NULL) || (strchr(url, '#') != NULL) || (strchr(url, '\\') != NULL))
    {
        generator_error(ctx, st->lineno, "path '%s' must be a clean absolute path", url);
        return -1;
    }

    if ((url[1] != '\0') && (url[strlen(url) - 1u] == '/'))
    {
        generator_error(ctx, st->lineno, "path '%s' must not end with '/'", url);
        return -1;
    }

    return 0;
}

int validate_route_url(Generator *ctx, ParseState *st, const char *url)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(url != NULL);

    if (route_url_shape_valid(ctx, st, url) != 0) { return -1; }

    Placeholders seen  = {0};
    const char  *p     = url + 1;
    int          count = 0;

    while (*p != '\0')
    {
        if (count >= GENERATOR_MAX_ROUTE_SEGMENTS)
        {
            generator_error(ctx, st->lineno, "too many path segments");
            return -1;
        }

        const char *slash = strchr(p, '/');
        size_t      n     = (slash != NULL) ? (size_t)(slash - p) : strlen(p);

        if (validate_route_segment(ctx, st, url, p, n, &seen) != 0) { return -1; }

        count++;
        if (slash == NULL) { break; }
        p = slash + 1;
    }

    return 0;
}

static int parse_collection_return(Generator *ctx, ParseState *st, ParsedRoute *r, char *return_type)
{
    assert(ctx != NULL);
    assert(r != NULL);
    assert(return_type != NULL);

    if ((strncmp(return_type, "$list(", 6u) != 0) && (strncmp(return_type, "$page(", 6u) != 0)) { return 0; }

    size_t n = strlen(return_type);
    if ((n < 8u) || (return_type[n - 1u] != ')'))
    {
        generator_error(ctx, st->lineno, "use $list(Type) or $page(Type)");
        return -1;
    }

    r->collection = (return_type[1] == 'l') ? 1 : 2;
    return_type[n - 1u] = '\0';

    if (is_safe_identifier(return_type + 6) == 0)
    {
        generator_error(ctx, st->lineno, "invalid collection type");
        return -1;
    }

    (void)snprintf(r->return_type, sizeof(r->return_type), "%s", return_type + 6);

    return 0;
}

static void classify_params(ParsedRoute *r, int *bodies, int *paths)
{
    assert(r != NULL);
    assert(bodies != NULL);
    assert(paths != NULL);

    *bodies = 0;
    *paths  = 0;

    for (int k = 0; (k < r->param_count) && (k < GENERATOR_MAX_PARAMS); k++)
    {
        if (r->params[k].is_body == 1)
        {
            (*bodies)++;
            continue;
        }

        if (url_has_param(r->url, r->params[k].name) == 1)
        {
            r->params[k].is_query  = 0;
            r->params[k].path_slot = url_param_slot(r->url, r->params[k].name);
            (*paths)++;
        }
        else
        {
            r->params[k].is_query = 1;
        }
    }
}

static int route_params_consistent(Generator *ctx, ParseState *st, ParsedRoute *r)
{
    assert(ctx != NULL);
    assert(r != NULL);

    int bodies = 0;
    int paths  = 0;
    classify_params(r, &bodies, &paths);

    if (bodies > 1)
    {
        generator_error(ctx, st->lineno, "%s takes %d bodies; only one is allowed", r->function_name, bodies);
        return -1;
    }

    int wanted = count_url_params(r->url);
    if (wanted != paths)
    {
        generator_error(ctx, st->lineno, "path '%s' has %d {…} segment(s)" " but %s takes %d with a matching name", r->url, wanted, r->function_name, paths);
        return -1;
    }

    return 0;
}

static void take_pending_annotations(Generator *ctx, ParseState *st, ParsedRoute *r)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(r != NULL);

    r->authenticated = st->pending_auth;
    r->public_route  = st->pending_public;
    (void)snprintf(r->role, sizeof(r->role), "%s", st->pending_role);

    if (r->public_route && (r->authenticated || r->role[0])) { generator_error(ctx, st->lineno, "$public cannot be combined with authentication"); }

    r->transactional = st->pending_transactional;
    (void)snprintf(r->produces, sizeof(r->produces), "%s", st->pending_produces);

    st->pending_produces[0]   = '\0';
    st->pending_auth          = 0;
    st->pending_public        = 0;
    st->pending_role[0]       = '\0';
    st->pending_transactional = 0;
}

static int parse_route_signature(Generator *ctx, ParseState *st, const char *line, char *function_name, char *return_type, size_t cap)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(line != NULL);

    const char *last_close = strrchr(line, ')');
    if ((last_close == NULL) || (last_close[1] != '\0'))
    {
        generator_error(ctx, st->lineno, "the handler signature must be alone on its line");
        return -1;
    }

    if (extract_return_type(line, return_type, cap) != 0)
    {
        generator_error(ctx, st->lineno, "after %s(\"%s\") expected 'type name(void)'", st->route_method, st->route_url);
        return -1;
    }

    if (extract_function_name(line, function_name, cap) != 0)
    {
        generator_error(ctx, st->lineno, "after %s(\"%s\") expected a function signature", st->route_method, st->route_url);
        return -1;
    }

    return 0;
}

void parse_route(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);

    st->route_armed = 0;

    if (ctx->route_count >= GENERATOR_MAX_ROUTES)
    {
        generator_error(ctx, st->lineno, "too many routes (limit %d)", GENERATOR_MAX_ROUTES);
        return;
    }

    char function_name[GENERATOR_MAX_NAME];
    char return_type[GENERATOR_MAX_NAME];
    if (parse_route_signature(ctx, st, line, function_name, return_type, sizeof(return_type)) != 0) { return; }

    ParsedRoute *r = &ctx->routes[ctx->route_count];
    memset(r, 0, sizeof(*r));
    (void)snprintf(r->method, sizeof(r->method), "%s", st->route_method);
    (void)snprintf(r->url, sizeof(r->url), "%s", st->route_url);
    (void)snprintf(r->function_name, sizeof(r->function_name), "%s", function_name);
    (void)snprintf(r->return_type, sizeof(r->return_type), "%s", return_type);

    if (parse_collection_return(ctx, st, r, return_type) != 0) { return; }

    r->line       = st->lineno;
    r->file_index = ctx->file_count - 1;

    if (extract_params(ctx, st, line, r, 0) != 0) { return; }
    if (route_params_consistent(ctx, st, r) != 0) { return; }

    take_pending_annotations(ctx, st, r);

    ctx->route_count++;

    if (ctx->file_count > 0) { ctx->files[ctx->file_count - 1].has_route = 1; }
}

void parse_service(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(line != NULL);

    if (ctx->service_count >= GENERATOR_MAX_SERVICES)
    {
        generator_error(ctx, st->lineno, "too many transactional services (limit %d)", GENERATOR_MAX_SERVICES);
        st->pending_transactional = 0;
        return;
    }
    const char *last_close = strrchr(line, ')');
    if (last_close == NULL || last_close[1] != '\0')
    {
        generator_error(ctx, st->lineno, "after $transactional expected a service signature alone on its line");
        st->pending_transactional = 0;
        return;
    }

    ParsedService *service = &ctx->services[ctx->service_count];
    ParsedRoute temporary;
    memset(service, 0, sizeof(*service));
    memset(&temporary, 0, sizeof(temporary));
    if (extract_return_type(line, service->return_type, sizeof(service->return_type)) != 0 || extract_function_name(line, service->function_name, sizeof(service->function_name)) != 0)
    {
        generator_error(ctx, st->lineno, "a transactional service requires a plain signature without pointers");
        st->pending_transactional = 0;
        return;
    }
    if (extract_params(ctx, st, line, &temporary, 1) != 0)
    {
        st->pending_transactional = 0;
        return;
    }
    service->param_count = temporary.param_count;
    memcpy(service->params, temporary.params, sizeof(service->params));
    service->line = st->lineno;
    service->file_index = ctx->file_count - 1;
    ctx->service_count++;
    if (ctx->file_count > 0) { ctx->files[ctx->file_count - 1].has_service = 1; }
    st->pending_transactional = 0;
}

void guard_transaction(Generator *ctx, ParseState *st, const char *line)
{
    assert(ctx != NULL);
    assert(st != NULL);
    assert(line != NULL);

    int opens  = 0;
    int closes = 0;
    for (int i = 0; (i < GENERATOR_MAX_LINE) && (line[i] != '\0'); i++)
    {
        if (line[i] == '{') { opens++; }
        if (line[i] == '}') { closes++; }
    }

    if ((st->transaction_depth > 0) && (st->brace_depth >= st->transaction_depth) && (strstr(line, "return") != NULL) && (strstr(line, "$throw") == NULL))
    {
        generator_error(ctx, st->lineno, "do not use return inside $transaction { }:" " the COMMIT would never run and data would be lost." " Use $throw to fail, or $transactional on the handler");
    }

    const char *found = strstr(line, "$transaction");
    if (found != NULL)
    {
        char after = found[12];
        if ((isalnum((unsigned char)after) == 0) && (after != '_')) { st->transaction_block_armed = 1; }
    }

    st->brace_depth += opens;

    if ((st->transaction_block_armed == 1) && (opens > 0))
    {
        st->transaction_depth = st->brace_depth;
        st->transaction_block_armed   = 0;
    }

    st->brace_depth -= closes;

    if ((st->transaction_depth > 0) && (st->brace_depth < st->transaction_depth)) { st->transaction_depth = 0; }
    if (st->brace_depth < 0) { st->brace_depth = 0; }
}
