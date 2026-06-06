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

