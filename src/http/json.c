#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "json.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
    OBJ_KEY_OR_END = 0, OBJ_KEY, OBJ_COLON, OBJ_VALUE, OBJ_NEXT,
    ARR_VALUE_OR_END, ARR_VALUE, ARR_NEXT
} FrameState;

typedef struct
{
    int token;
    JsonType type;
    FrameState state;
    int members;
} Frame;

static int token_alloc(JsonToken *toks, int max, int *count, JsonType type, int start, int parent)
{
    if ((*count < 0) || (*count >= max)) { return -1; }
    int n = *count;
    toks[n].type = type;
    toks[n].start = start;
    toks[n].end = -1;
    toks[n].parent = parent;
    (*count)++;
    return n;
}

static int is_space(unsigned char c)
{
    return ((c == ' ') || (c == '\t') || (c == '\r') || (c == '\n')) ? 1 : 0;
}

static int hex_digit(unsigned char c)
{
    if ((c >= '0') && (c <= '9')) { return (int)(c - '0'); }
    if ((c >= 'a') && (c <= 'f')) { return (int)(c - 'a') + 10; }
    if ((c >= 'A') && (c <= 'F')) { return (int)(c - 'A') + 10; }
    return -1;
}

static int read_hex4(const char *js, size_t len, size_t at, unsigned *out)
{
    if ((at > len) || ((len - at) < 4u)) { return -1; }
    unsigned value = 0u;
    for (size_t i = 0u; i < 4u; i++)
    {
        int h = hex_digit((unsigned char)js[at + i]);
        if (h < 0) { return -1; }
        value = (value << 4u) | (unsigned)h;
    }
    *out = value;
    return 0;
}

static int utf8_advance(const char *js, size_t len, size_t *at)
{
    size_t i = *at;
    unsigned char a = (unsigned char)js[i];
    size_t n;
    unsigned char min = 0x80u, max = 0xBFu;

    if ((a >= 0xC2u) && (a <= 0xDFu)) { n = 1u; }
    else if ((a >= 0xE0u) && (a <= 0xEFu))
    {
        n = 2u;
        if (a == 0xE0u) { min = 0xA0u; }
        if (a == 0xEDu) { max = 0x9Fu; }
    }
    else if ((a >= 0xF0u) && (a <= 0xF4u))
    {
        n = 3u;
        if (a == 0xF0u) { min = 0x90u; }
        if (a == 0xF4u) { max = 0x8Fu; }
    }
    else { return -1; }

    if ((i + n) >= len) { return -1; }
    unsigned char b = (unsigned char)js[i + 1u];
    if ((b < min) || (b > max)) { return -1; }
    for (size_t k = 2u; k <= n; k++)
    {
        unsigned char c = (unsigned char)js[i + k];
        if ((c < 0x80u) || (c > 0xBFu)) { return -1; }
    }
    *at = i + n + 1u;
    return 0;
}

static int token_string(const char *js, size_t len, size_t *at, JsonToken *toks, int max, int *count, int parent)
{
    int t = token_alloc(toks, max, count, JSON_STR, (int)(*at + 1u), parent);
    if (t < 0) { return -1; }

    size_t i = *at + 1u;
    while (i < len)
    {
        unsigned char c = (unsigned char)js[i];
        if (c == '"')
        {
            toks[t].end = (int)i;
            *at = i + 1u;
            return t;
        }
        if (c == '\\')
        {
            i++;
            if (i >= len) { return -1; }
            c = (unsigned char)js[i];
            if ((c == '"') || (c == '\\') || (c == '/') || (c == 'b') || (c == 'f') || (c == 'n') || (c == 'r') || (c == 't'))
            {
                i++;
                continue;
            }
            if (c != 'u') { return -1; }
            unsigned first = 0u;
            if (read_hex4(js, len, i + 1u, &first) != 0) { return -1; }
            i += 5u;
            if ((first >= 0xD800u) && (first <= 0xDBFFu))
            {
                unsigned second = 0u;
                if (((len - i) < 6u) || (js[i] != '\\') || (js[i + 1u] != 'u') || (read_hex4(js, len, i + 2u, &second) != 0) || (second < 0xDC00u) || (second > 0xDFFFu)) { return -1; }
                i += 6u;
            }
            else if ((first >= 0xDC00u) && (first <= 0xDFFFu))
            {
                return -1;
            }
            continue;
        }
        if (c < 0x20u) { return -1; }
        if (c >= 0x80u)
        {
            if (utf8_advance(js, len, &i) != 0) { return -1; }
        }
        else { i++; }
    }
    return -1;
}

static int is_delimiter(const char *js, size_t len, size_t at)
{
    if (at >= len) { return 1; }
    unsigned char c = (unsigned char)js[at];
    return ((is_space(c) == 1) || (c == ',') || (c == ']') || (c == '}')) ? 1 : 0;
}

static int scan_number(const char *js, size_t len, size_t *at)
{
    size_t i = *at;
    if ((i < len) && (js[i] == '-')) { i++; }
    if (i >= len) { return -1; }
    if (js[i] == '0')
    {
        i++;
        if ((i < len) && (js[i] >= '0') && (js[i] <= '9')) { return -1; }
    }
    else if ((js[i] >= '1') && (js[i] <= '9'))
    {
        do { i++; } while ((i < len) && (js[i] >= '0') && (js[i] <= '9'));
    }
    else { return -1; }

    if ((i < len) && (js[i] == '.'))
    {
        i++;
        if ((i >= len) || (js[i] < '0') || (js[i] > '9')) { return -1; }
        do { i++; } while ((i < len) && (js[i] >= '0') && (js[i] <= '9'));
    }
    if ((i < len) && ((js[i] == 'e') || (js[i] == 'E')))
    {
        i++;
        if ((i < len) && ((js[i] == '+') || (js[i] == '-'))) { i++; }
        if ((i >= len) || (js[i] < '0') || (js[i] > '9')) { return -1; }
        do { i++; } while ((i < len) && (js[i] >= '0') && (js[i] <= '9'));
    }
    if (is_delimiter(js, len, i) == 0) { return -1; }
    *at = i;
    return 0;
}

static int token_primitive(const char *js, size_t len, size_t *at, JsonToken *toks, int max, int *count, int parent)
{
    size_t start = *at;
    if ((js[start] == 't') && ((len - start) >= 4u) && (memcmp(js + start, "true", 4u) == 0)) { *at += 4u; }
    else if ((js[start] == 'f') && ((len - start) >= 5u) && (memcmp(js + start, "false", 5u) == 0)) { *at += 5u; }
    else if ((js[start] == 'n') && ((len - start) >= 4u) && (memcmp(js + start, "null", 4u) == 0)) { *at += 4u; }
    else if (scan_number(js, len, at) != 0) { return -1; }
    if (is_delimiter(js, len, *at) == 0) { return -1; }

    int t = token_alloc(toks, max, count, JSON_PRIM, (int)start, parent);
    if (t < 0) { return -1; }
    toks[t].end = (int)*at;
    return t;
}

static int scan_value(const char *js, size_t len, size_t *at, JsonToken *toks, int max, int *count, int parent, Frame *stack, int *depth)
{
    if (*at >= len) { return -1; }
    char c = js[*at];
    if (c == '"') { return (token_string(js, len, at, toks, max, count, parent) < 0) ? -1 : 0; }
    if ((c == '{') || (c == '['))
    {
        if (*depth >= JSON_MAX_DEPTH) { return -1; }
        JsonType type = (c == '{') ? JSON_OBJ : JSON_ARR;
        int t = token_alloc(toks, max, count, type, (int)*at, parent);
        if (t < 0) { return -1; }
        stack[*depth].token = t;
        stack[*depth].type = type;
        stack[*depth].state = (type == JSON_OBJ) ? OBJ_KEY_OR_END
                                                 : ARR_VALUE_OR_END;
        stack[*depth].members = 0;
        (*depth)++;
        (*at)++;
        return 0;
    }
    return (token_primitive(js, len, at, toks, max, count, parent) < 0) ? -1 : 0;
}

static void close_container(JsonToken *toks, const Frame *f, size_t *i, int *depth, int *done)
{
    assert(toks != NULL);
    assert(f != NULL);

    toks[f->token].end = (int)(++(*i));
    (*depth)--;
    if (*depth == 0) { *done = 1; }
}

static int duplicate_key(const char *js, const JsonToken *toks, int parent, int key)
{
    assert(js != NULL);
    assert(toks != NULL);

    char decoded_key[JSON_MAX_KEY_BYTES];
    json_copy_str(js, &toks[key], decoded_key, sizeof(decoded_key));

    int direct = 0;
    for (int prior = parent + 1; prior < key; prior++)
    {
        if (toks[prior].parent != parent) { continue; }

        if (((direct & 1) == 0) && (toks[prior].type == JSON_STR))
        {
            char decoded_prior[JSON_MAX_KEY_BYTES];
            json_copy_str(js, &toks[prior], decoded_prior, sizeof(decoded_prior));
            if (strcmp(decoded_prior, decoded_key) == 0) { return 1; }
        }
        direct++;
    }

    return 0;
}

static int scan_object_key(const char *js, size_t len, size_t *i, JsonToken *toks, int max, int *count, Frame *f, int *depth, int *done)
{
    assert(js != NULL);
    assert(f != NULL);

    if (js[*i] == '}')
    {
        if (f->state == OBJ_KEY) { return -1; }
        close_container(toks, f, i, depth, done);
        return 0;
    }
    if (js[*i] != '"') { return -1; }

    int key = token_string(js, len, i, toks, max, count, f->token);
    if ((key < 0) || (f->members >= JSON_MAX_OBJECT_KEYS)) { return -1; }

    int key_len = toks[key].end - toks[key].start;
    if ((key_len < 0) || (key_len >= JSON_MAX_KEY_BYTES)) { return -1; }
    if (duplicate_key(js, toks, f->token, key) != 0) { return -1; }

    f->members++;
    f->state = OBJ_COLON;

    return 0;
}

static int scan_array_element(const char *js, size_t len, size_t *i, JsonToken *toks, int max, int *count, Frame *f, Frame *stack, int *depth, int *done)
{
    assert(js != NULL);
    assert(f != NULL);

    if (js[*i] == ']')
    {
        if (f->state == ARR_VALUE) { return -1; }
        close_container(toks, f, i, depth, done);
        return 0;
    }

    f->state = ARR_NEXT;

    return scan_value(js, len, i, toks, max, count, f->token, stack, depth);
}

static int scan_separator(const char *js, size_t *i, Frame *f, JsonToken *toks, int *depth, int *done, char closer, FrameState next)
{
    assert(js != NULL);
    assert(f != NULL);

    if (js[*i] == ',')
    {
        f->state = next;
        (*i)++;
        return 0;
    }
    if (js[*i] != closer) { return -1; }

    close_container(toks, f, i, depth, done);

    return 0;
}

static int scan_frame(const char *js, size_t len, size_t *i, JsonToken *toks, int max, int *count, Frame *stack, int *depth, int *done)
{
    assert(js != NULL);
    assert(stack != NULL);

    Frame *f = &stack[*depth - 1];

    switch (f->state)
    {
        case OBJ_KEY_OR_END:
        case OBJ_KEY:
            return scan_object_key(js, len, i, toks, max, count, f, depth, done);

        case OBJ_COLON:
            if (js[*i] != ':') { return -1; }
            f->state = OBJ_VALUE;
            (*i)++;
            return 0;

        case OBJ_VALUE:
            f->state = OBJ_NEXT;
            return scan_value(js, len, i, toks, max, count, f->token, stack, depth);

        case OBJ_NEXT:
            return scan_separator(js, i, f, toks, depth, done, '}', OBJ_KEY);

        case ARR_VALUE_OR_END:
        case ARR_VALUE:
            return scan_array_element(js, len, i, toks, max, count, f, stack, depth, done);

        case ARR_NEXT:
            return scan_separator(js, i, f, toks, depth, done, ']', ARR_VALUE);

        default:
            break;
    }

    return -1;
}

int json_parse(const char *js, size_t len, JsonToken *toks, int max)
{
    if ((js == NULL) || (toks == NULL) || (max <= 0) || (len == 0u) || (len > (size_t)INT_MAX)) { return -1; }

    Frame  stack[JSON_MAX_DEPTH];
    int    depth = 0, count = 0, seen = 0, done = 0;
    size_t i = 0u;

    while (i < len)
    {
        while ((i < len) && (is_space((unsigned char)js[i]) == 1)) { i++; }
        if (i >= len) { break; }

        if (depth == 0)
        {
            if ((seen != 0) || (done != 0)) { return -1; }
            seen = 1;
            if (scan_value(js, len, &i, toks, max, &count, -1, stack, &depth) != 0) { return -1; }
            if (depth == 0) { done = 1; }
            continue;
        }

        if (scan_frame(js, len, &i, toks, max, &count, stack, &depth, &done) != 0) { return -1; }
    }

    return ((seen == 1) && (done == 1) && (depth == 0)) ? count : -1;
}

static size_t utf8_encode(unsigned cp, char *out, size_t cap)
{
    if ((cp <= 0x7Fu) && (cap >= 1u)) { out[0] = (char)cp; return 1u; }
    if ((cp <= 0x7FFu) && (cap >= 2u))
    {
        out[0] = (char)(0xC0u | (cp >> 6u));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    if ((cp <= 0xFFFFu) && (cap >= 3u))
    {
        out[0] = (char)(0xE0u | (cp >> 12u));
        out[1] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3u;
    }
    if ((cp <= 0x10FFFFu) && (cap >= 4u))
    {
        out[0] = (char)(0xF0u | (cp >> 18u));
        out[1] = (char)(0x80u | ((cp >> 12u) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6u) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4u;
    }
    return 0u;
}

void json_copy_str(const char *js, const JsonToken *t, char *out, size_t outsz)
{
    assert(js != NULL);
    assert(t != NULL);
    assert(out != NULL);
    if (outsz == 0u) { return; }
    if (t->start < 0 || t->end < t->start)
    {
        out[0] = '\0';
        return;
    }

    size_t i = (size_t)t->start, end = (size_t)t->end, o = 0u;
    while ((i < end) && ((o + 1u) < outsz))
    {
        char c = js[i++];
        if ((t->type != JSON_STR) || (c != '\\')) { out[o++] = c; continue; }
        if (i >= end) { break; }
        c = js[i++];
        if (c == 'b') { out[o++] = '\b'; }
        else if (c == 'f') { out[o++] = '\f'; }
        else if (c == 'n') { out[o++] = '\n'; }
        else if (c == 'r') { out[o++] = '\r'; }
        else if (c == 't') { out[o++] = '\t'; }
        else if ((c == '"') || (c == '\\') || (c == '/')) { out[o++] = c; }
        else if (c == 'u')
        {
            unsigned first = 0u, cp;
            if (read_hex4(js, end, i, &first) != 0) { break; }
            i += 4u;
            cp = first;
            if ((first >= 0xD800u) && (first <= 0xDBFFu) && ((end - i) >= 6u) && (js[i] == '\\') && (js[i + 1u] == 'u'))
            {
                unsigned second = 0u;
                if (read_hex4(js, end, i + 2u, &second) != 0) { break; }
                cp = 0x10000u + ((first - 0xD800u) << 10u) + (second - 0xDC00u);
                i += 6u;
            }
            o += utf8_encode(cp, out + o, outsz - o - 1u);
        }
        else { break; }
    }
    out[o] = '\0';
}

int json_object_get(const char *js, const JsonToken *toks, int ntok, int obj, const char *key)
{
    assert(js != NULL);
    assert(toks != NULL);
    assert(key != NULL);
    if ((obj < 0) || (obj >= ntok) || (toks[obj].type != JSON_OBJ)) { return -1; }

    size_t klen = strlen(key);
    if (klen >= JSON_MAX_KEY_BYTES) { return -1; }
    int direct = 0;
    for (int i = obj + 1; (i < ntok) && (toks[i].start < toks[obj].end); i++)
    {
        if (toks[i].parent != obj) { continue; }
        if (((direct & 1) == 0) && (toks[i].type == JSON_STR))
        {
            int raw_len = toks[i].end - toks[i].start;
            if ((raw_len >= 0) && (raw_len < JSON_MAX_KEY_BYTES))
            {
                char decoded[JSON_MAX_KEY_BYTES];
                json_copy_str(js, &toks[i], decoded, sizeof(decoded));
                if ((strlen(decoded) == klen) && (memcmp(decoded, key, klen) == 0)) { return ((i + 1) < ntok) ? i + 1 : -1; }
            }
        }
        direct++;
    }
    return -1;
}

int json_path_get(const char *js, const JsonToken *toks, int ntok, int root, const char *path)
{
    assert(js != NULL);
    assert(toks != NULL);
    assert(path != NULL);
    if (root < 0 || root >= ntok) { return -1; }
    int cur = root;
    const char *p = path;
    for (int guard = 0; (*p != '\0') && (cur >= 0) && (guard < JSON_MAX_DEPTH); guard++)
    {
        char seg[JSON_PATH_SEGMENT];
        size_t n = 0u;
        while ((*p != '\0') && (*p != '.') && ((n + 1u) < sizeof(seg))) { seg[n++] = *p++; }
        if ((*p != '\0') && (*p != '.')) { return -1; }
        seg[n] = '\0';
        if (*p == '.') { p++; }
        if (cur >= ntok) { return -1; }
        if (toks[cur].type == JSON_OBJ)
        {
            cur = json_object_get(js, toks, ntok, cur, seg);
        }
        else if (toks[cur].type == JSON_ARR)
        {
            char *endp = NULL;
            errno = 0;
            long want = strtol(seg, &endp, 10);
            if ((errno != 0) || (endp == seg) || (*endp != '\0') || (want < 0) || (want > INT_MAX)) { return -1; }
            int idx = 0, found = -1;
            for (int i = cur + 1; (i < ntok) && (toks[i].start < toks[cur].end); i++)
            {
                if (toks[i].parent != cur) { continue; }
                if (idx == (int)want) { found = i; break; }
                idx++;
            }
            cur = found;
        }
        else { return -1; }
    }
    return (*p == '\0') ? cur : -1;
}

static int token_text(const char *js, const JsonToken *t, char *buf, size_t cap)
{
    if ((js == NULL) || (t == NULL) || (buf == NULL) || (cap < 2u) || (t->type != JSON_PRIM) || (t->start < 0) || (t->end <= t->start)) { return -1; }
    size_t n = (size_t)(t->end - t->start);
    if (n >= cap) { return -1; }
    memcpy(buf, js + t->start, n);
    buf[n] = '\0';
    return 0;
}

int json_token_long_checked(const char *js, const JsonToken *t, long *out)
{
    char buf[128], *end = NULL;
    if ((out == NULL) || (token_text(js, t, buf, sizeof(buf)) != 0)) { return -1; }
    errno = 0;
    long value = strtol(buf, &end, 10);
    if ((errno == ERANGE) || (end == buf) || (*end != '\0')) { return -1; }
    *out = value;
    return 0;
}

int json_token_double_checked(const char *js, const JsonToken *t, double *out)
{
    char buf[128], *end = NULL;
    if ((out == NULL) || (token_text(js, t, buf, sizeof(buf)) != 0)) { return -1; }
    errno = 0;
    double value = strtod(buf, &end);
    if ((errno == ERANGE) || (end == buf) || (*end != '\0') || !isfinite(value)) { return -1; }
    *out = value;
    return 0;
}

int json_token_bool_checked(const char *js, const JsonToken *t, int *out)
{
    if ((js == NULL) || (t == NULL) || (out == NULL) || (t->type != JSON_PRIM) || (t->start < 0) || (t->end < t->start))
    { return -1; }
    size_t n = (size_t)(t->end - t->start);
    if ((n == 4u) && (memcmp(js + t->start, "true", 4u) == 0))
    {
        *out = 1;
        return 0;
    }
    if ((n == 5u) && (memcmp(js + t->start, "false", 5u) == 0))
    {
        *out = 0;
        return 0;
    }
    return -1;
}

long json_token_long(const char *js, const JsonToken *t)
{
    long value = 0L;
    (void)json_token_long_checked(js, t, &value);
    return value;
}
