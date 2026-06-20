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

static int load_template(const char *name, char *out, size_t cap, size_t *len)
{
    assert(name != NULL);
    assert(out != NULL);
    assert(len != NULL);

    if ((template_enabled() == 0) || (name_safe(name) == 0)) { return -1; }

    char candidate[TEMPLATE_MAX_PATH];
    int  n = snprintf(candidate, sizeof(candidate), "%s/%s", g_root, name);
    if ((n <= 0) || ((size_t)n >= sizeof(candidate))) { return -1; }

    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) { return -1; }
    if ((strncmp(resolved, g_root, g_root_len) != 0) || (resolved[g_root_len] != '/')) { return -1; }

    FILE *file = fopen(resolved, "rb");
    if (file == NULL) { return -1; }

    struct stat info;
    if ((fstat(fileno(file), &info) != 0) || (!S_ISREG(info.st_mode)) || (info.st_size < 0) || ((size_t)info.st_size >= cap))
    {
        (void)fclose(file);
        return -1;
    }

    size_t want = (size_t)info.st_size;
    size_t got  = fread(out, 1u, want, file);
    (void)fclose(file);

    if (got != want) { return -1; }

    out[got] = '\0';
    *len     = got;

    return 0;
}

static void write_row(Writer *writer, const char *block, size_t block_len, const TypeInfo *type, const void *row, int index, int total)
{
    assert(writer != NULL);
    assert(block != NULL);

    size_t at = 0u;
    while ((at < block_len) && (writer->overflow == 0))
    {
        const char *open = find_bytes(block + at, block_len - at, "{{", 2u);
        if (open == NULL)
        {
            write_raw(writer, block + at, block_len - at);
            return;
        }

        write_raw(writer, block + at, (size_t)(open - (block + at)));

        const char *close = find_bytes(open, block_len - (size_t)(open - block), "}}", 2u);
        if (close == NULL)
        {
            write_raw(writer, open, block_len - (size_t)(open - block));
            return;
        }

        char key[64];
        size_t key_len = (size_t)(close - open) - 2u;
        if (key_len >= sizeof(key)) { key_len = sizeof(key) - 1u; }
        memcpy(key, open + 2, key_len);
        key[key_len] = '\0';

        char value[TEMPLATE_MAX_VALUE];
        if (strcmp(key, "index") == 0)
        {
            (void)snprintf(value, sizeof(value), "%d", index);
            write_escaped(writer, value);
        }
        else if (strcmp(key, "count") == 0)
        {
            (void)snprintf(value, sizeof(value), "%d", total);
            write_escaped(writer, value);
        }
        else if ((row != NULL) && (field_text(type, row, key, value, sizeof(value)) == 0))
        {
            write_escaped(writer, value);
        }

        at = (size_t)(close - block) + 2u;
    }
}

static str finish(const Writer *writer)
{
    assert(writer != NULL);

    if (writer->overflow != 0)
    {
        request_fail(500, "rendered page exceeds the response limit");
        return "";
    }

    return writer->out;
}

str render_template(str name, RowList rows)
{
    if (name == NULL)
    {
        request_fail(500, "template name missing");
        return "";
    }

    static _Thread_local char source[TEMPLATE_MAX_BYTES];
    size_t source_len = 0u;

    if (load_template(name, source, sizeof(source), &source_len) != 0)
    {
        request_fail(500, "template not found or too large");
        return "";
    }

    char *out = request_alloc((size_t)TEMPLATE_MAX_OUT);
    if (out == NULL) { return ""; }

    Writer writer = { out, (size_t)TEMPLATE_MAX_OUT, 0u, 0 };
    out[0] = '\0';

    const char *each_open  = find_bytes(source, source_len, "{{#each}}", 9u);
    const char *each_close = find_bytes(source, source_len, "{{/each}}", 9u);

    if ((each_open == NULL) || (each_close == NULL) || (each_close < each_open))
    {
        write_row(&writer, source, source_len, rows.type, NULL, 0, rows.count);
        return finish(&writer);
    }

    size_t head_len  = (size_t)(each_open - source);
    const char *block = each_open + 9;
    size_t block_len = (size_t)(each_close - block);
    const char *tail = each_close + 9;
    size_t tail_len  = source_len - (size_t)(tail - source);

    write_row(&writer, source, head_len, rows.type, NULL, 0, rows.count);

    if ((rows.type != NULL) && (rows.items != NULL))
    {
        for (int i = 0; (i < rows.count) && (i < PAGE_MAX); i++)
        {
            const char *row = (const char *)rows.items +
                              ((size_t)i * rows.type->size);
            write_row(&writer, block, block_len, rows.type, row, i, rows.count);
        }
    }

    write_row(&writer, tail, tail_len, rows.type, NULL, 0, rows.count);

    return finish(&writer);
}
