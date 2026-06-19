#include "template.h"
#include "config.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char   g_root[TEMPLATE_MAX_PATH];
static size_t g_root_len;

static const char *find_bytes(const char *haystack, size_t haystack_len, const char *needle, size_t needle_len)
{
    assert(haystack != NULL);
    assert(needle != NULL);

    if ((needle_len == 0u) || (haystack_len < needle_len)) { return NULL; }

    for (size_t i = 0u; i <= (haystack_len - needle_len); i++)
    {
        if (memcmp(haystack + i, needle, needle_len) == 0) { return haystack + i; }
    }

    return NULL;
}

typedef struct
{
    char  *out;
    size_t cap;
    size_t used;
    int    overflow;
} Writer;

static void write_raw(Writer *writer, const char *text, size_t len)
{
    assert(writer != NULL);

    if (writer->overflow != 0) { return; }
    if (len >= (writer->cap - writer->used))
    {
        writer->overflow = 1;
        return;
    }

    memcpy(writer->out + writer->used, text, len);
    writer->used += len;
    writer->out[writer->used] = '\0';
}

static void write_escaped(Writer *writer, const char *text)
{
    assert(writer != NULL);

    for (size_t i = 0u; (text[i] != '\0') && (writer->overflow == 0); i++)
    {
        switch (text[i])
        {
            case '&':  write_raw(writer, "&amp;", 5u);  break;
            case '<':  write_raw(writer, "&lt;", 4u);   break;
            case '>':  write_raw(writer, "&gt;", 4u);   break;
            case '"':  write_raw(writer, "&quot;", 6u); break;
            case '\'': write_raw(writer, "&#39;", 5u);  break;
            default:   write_raw(writer, &text[i], 1u); break;
        }
    }
}

int template_init(void)
{
    g_root[0]  = '\0';
    g_root_len = 0u;

    const char *configured = config_str("templates.root", "");
    if ((configured == NULL) || (configured[0] == '\0')) { return 0; }

    char resolved[PATH_MAX];
    if (realpath(configured, resolved) == NULL)
    {
        (void)fprintf(stderr, "gargantua: templates.root '%s' does not exist\n", configured);
        return -1;
    }

    struct stat info;
    if ((stat(resolved, &info) != 0) || (!S_ISDIR(info.st_mode)))
    {
        (void)fprintf(stderr, "gargantua: templates.root '%s' is not a directory\n", configured);
        return -1;
    }

    size_t n = strlen(resolved);
    while ((n > 1u) && (resolved[n - 1u] == '/')) { n--; }
    if ((n == 0u) || (n + 1u >= sizeof(g_root))) { return -1; }

    memcpy(g_root, resolved, n);
    g_root[n]  = '\0';
    g_root_len = n;

    return 0;
}

int template_enabled(void)
{
    return (g_root_len > 0u) ? 1 : 0;
}

static int name_safe(const char *name)
{
    assert(name != NULL);

    size_t n = strlen(name);
    if ((n == 0u) || (n > 128u)) { return 0; }

    for (size_t i = 0u; i < n; i++)
    {
        char c = name[i];
        int ok = (((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '.') || (c == '_') || (c == '-'));
        if (ok == 0) { return 0; }
        if ((c == '.') && (name[i + 1u] == '.')) { return 0; }
    }

    return 1;
}

