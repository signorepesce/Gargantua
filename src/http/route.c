#include "route.h"
#include <assert.h>
#include <limits.h>
#include <string.h>

#define ROUTE_SEGMENT_MAX  66

static size_t next_segment(const char *s, size_t from, size_t *next)
{
    assert(s != NULL);
    assert(next != NULL);

    size_t i = from;
    while ((s[i] != '\0') && (s[i] != '/')) { i++; }
    *next = (s[i] == '/') ? (i + 1u) : i;
    return i - from;
}

static int path_matches(const char *pattern, const char *url, RequestParams *out)
{
    assert(pattern != NULL);
    assert(url != NULL);
    assert(out != NULL);

    out->path_count = 0;

    size_t pi = 0u;
    size_t ui = 0u;

    for (int guard = 0; guard < ROUTE_SEGMENT_MAX; guard++)
    {
        int p_end = (pattern[pi] == '\0') ? 1 : 0;
        int u_end = (url[ui] == '\0') ? 1 : 0;

        if ((p_end == 1) && (u_end == 1)) { return 1; }
        if ((p_end == 1) || (u_end == 1)) { return 0; }

        size_t pn = 0u;
        size_t un = 0u;
        size_t plen = next_segment(pattern, pi, &pn);
        size_t ulen = next_segment(url, ui, &un);

        if ((plen > 1u) && (pattern[pi] == '{') && (pattern[pi + plen - 1u] == '}'))
        {
            if (out->path_count >= PATH_PARAM_MAX) { return 0; }
            if (ulen == 0u) { return 0; }
            if (ulen >= (size_t)PARAM_VALUE_LEN)
            {
                out->invalid = 1;
                return -1;
            }
            memcpy(out->path_value[out->path_count], url + ui, ulen);
            out->path_value[out->path_count][ulen] = '\0';
            out->path_count++;
        }
        else
        {
            if (plen != ulen) { return 0; }
            if ((plen > 0u) && (memcmp(pattern + pi, url + ui, plen) != 0)) { return 0; }
        }

        pi = pn;
        ui = un;
    }

    return 0;
}

static int dynamic_segment_count(const char *pattern)
{
    int count = 0;
    for (size_t i = 0u; (i < ROUTE_MAX_URL) && (pattern[i] != '\0'); i++)
    {
        if (pattern[i] == '{') { count++; }
    }
    return count;
}

static void params_reset(RequestParams *p)
{
    assert(p != NULL);
    assert(PATH_PARAM_MAX > 0);

    p->path_count   = 0;
    p->query_count  = 0;
    p->invalid = 0;
}

static void params_copy(RequestParams *dst, const RequestParams *src)
{
    assert(dst != NULL);
    assert(src != NULL);

    dst->path_count   = src->path_count;
    dst->query_count  = src->query_count;
    dst->invalid = src->invalid;

    for (int i = 0; (i < src->path_count) && (i < PATH_PARAM_MAX); i++) { memcpy(dst->path_value[i], src->path_value[i], strlen(src->path_value[i]) + 1u); }
    for (int i = 0; (i < src->query_count) && (i < QUERY_PARAM_MAX); i++)
    {
        memcpy(dst->query_name[i], src->query_name[i], strlen(src->query_name[i]) + 1u);
        memcpy(dst->query_value[i], src->query_value[i], strlen(src->query_value[i]) + 1u);
    }
}

static const Route *route_find_exact(const char *method, const char *url, RequestParams *out)
{
    assert(method != NULL);
    assert(url != NULL);
    assert(out != NULL);

    const Route *table = route_table();
    int            n     = route_count();

    assert(table != NULL);
    assert(n >= 0);

    int saw_invalid = 0;
    int ambiguous = 0;
    int best_dynamic = INT_MAX;
    const Route *best = NULL;
    RequestParams best_params;
    params_reset(&best_params);
    for (int i = 0; i < n; i++)
    {
        const Route *r = &table[i];

        if (r->handler == NULL) { continue; }

        if ((r->method[0] != method[0]) || (strcmp(r->method, method) != 0)) { continue; }
        RequestParams candidate;
        params_reset(&candidate);
        int matched = path_matches(r->url, url, &candidate);
        if (matched == 1)
        {
            int dynamic = dynamic_segment_count(r->url);
            if (dynamic < best_dynamic)
            {
                best = r;
                params_copy(&best_params, &candidate);
                best_dynamic = dynamic;
                ambiguous = 0;
            }
            else if (dynamic == best_dynamic)
            {
                ambiguous = 1;
            }
        }
        if (matched < 0) { saw_invalid = 1; }
    }

    if ((best != NULL) && (ambiguous == 0))
    {
        params_copy(out, &best_params);
        return best;
    }
    params_reset(out);
    out->invalid = ((saw_invalid != 0) || (ambiguous != 0)) ? 1 : 0;
    return NULL;
}

const Route *route_find(const char *method, const char *url, RequestParams *out)
{
    const Route *route = route_find_exact(method, url, out);
    if ((route == NULL) && (out->invalid == 0) && (strcmp(method, "HEAD") == 0)) { route = route_find_exact("GET", url, out); }
    return route;
}

int route_allow(const char *url, char *out, size_t cap)
{
    static const char *const methods[] =
    {
        "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"
    };
    size_t at = 0u;
    int found = 0;
    int invalid = 0;
    if ((out == NULL) || (cap == 0u)) { return -1; }
    out[0] = '\0';
    for (size_t i = 0u; i < sizeof(methods) / sizeof(methods[0]); i++)
    {
        RequestParams params;
        const Route *route = route_find(methods[i], url, &params);
        if (route == NULL) { invalid |= params.invalid; }
        int options = (strcmp(methods[i], "OPTIONS") == 0);
        if ((route == NULL) && !(options && found)) { continue; }
        size_t len = strlen(methods[i]);
        size_t separator = (at > 0u) ? 2u : 0u;
        if (at + separator + len >= cap)
        {
            out[0] = '\0';
            return -1;
        }
        if (separator != 0u)
        {
            memcpy(out + at, ", ", separator);
            at += separator;
        }
        memcpy(out + at, methods[i], len + 1u);
        at += len;
        found = 1;
    }
    return found ? 1 : (invalid ? -1 : 0);
}
