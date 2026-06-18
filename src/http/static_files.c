#include "static_files.h"
#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int    root_directory = -1;
static char   g_root[STATIC_MAX_PATH];
static size_t g_root_len;
static char   g_prefix[STATIC_MAX_PATH];
static size_t g_prefix_len;

static const char *content_type_for(const char *path)
{
    assert(path != NULL);

    static const struct { const char *suffix; const char *type; } table[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".css",  "text/css; charset=utf-8" },
        { ".mjs",  "text/javascript; charset=utf-8" },
        { ".js",   "text/javascript; charset=utf-8" },
        { ".json", "application/json; charset=utf-8" },
        { ".map",  "application/json; charset=utf-8" },
        { ".xml",  "application/xml; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".csv",  "text/csv; charset=utf-8" },
        { ".svg",  "image/svg+xml" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".webp", "image/webp" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
        { ".wasm", "application/wasm" },
        { ".woff2","font/woff2" },
        { ".woff", "font/woff" },
        { ".ttf",  "font/ttf" }
    };

    size_t len = strlen(path);
    for (size_t i = 0u; i < (sizeof(table) / sizeof(table[0])); i++)
    {
        size_t n = strlen(table[i].suffix);
        if ((len > n) && (strcmp(path + len - n, table[i].suffix) == 0)) { return table[i].type; }
    }

    return "application/octet-stream";
}

int static_files_init(void)
{
    if (root_directory >= 0)
    {
        (void)close(root_directory);
        root_directory = -1;
    }
    g_root[0]    = '\0';
    g_root_len   = 0u;
    g_prefix[0]  = '\0';
    g_prefix_len = 0u;

    const char *configured = config_str("static.root", "");
    if ((configured == NULL) || (configured[0] == '\0')) { return 0; }

    char resolved[PATH_MAX];
    if (realpath(configured, resolved) == NULL)
    {
        (void)fprintf(stderr, "gargantua: static.root '%s' does not exist\n", configured);
        return -1;
    }

    struct stat info;
    if ((stat(resolved, &info) != 0) || (!S_ISDIR(info.st_mode)))
    {
        (void)fprintf(stderr, "gargantua: static.root '%s' is not a directory\n", configured);
        return -1;
    }

    size_t n = strlen(resolved);
    if ((n == 0u) || (n + 1u >= sizeof(g_root))) { return -1; }
    while ((n > 1u) && (resolved[n - 1u] == '/')) { resolved[--n] = '\0'; }

    memcpy(g_root, resolved, n + 1u);
    g_root_len = n;

    const char *prefix = config_str("static.prefix", "/static");
    size_t      p      = strlen(prefix);
    if ((p == 0u) || (prefix[0] != '/') || (p + 1u >= sizeof(g_prefix)))
    {
        (void)fprintf(stderr, "gargantua: invalid static.prefix\n");
        g_root[0]  = '\0';
        g_root_len = 0u;
        return -1;
    }
    while ((p > 0u) && (prefix[p - 1u] == '/')) { p--; }

    memcpy(g_prefix, prefix, p);
    g_prefix[p]  = '\0';
    g_prefix_len = p;
    root_directory = open(g_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    return root_directory < 0 ? -1 : 0;
}

int static_files_enabled(void)
{
    return (root_directory >= 0) ? 1 : 0;
}

static int strip_prefix(const char *url_path, const char **rest)
{
    assert(url_path != NULL);
    assert(rest != NULL);

    if (strncmp(url_path, g_prefix, g_prefix_len) != 0) { return -1; }

    const char *tail = url_path + g_prefix_len;
    if ((tail[0] != '\0') && (tail[0] != '/')) { return -1; }

    *rest = tail;

    return 0;
}

static int open_static_path(char *path)
{
    int directory = dup(root_directory);
    if (directory < 0) { return -1; }
    char *part = path;
    for (size_t step = 0u; step < STATIC_MAX_PATH; step++)
    {
        char *slash = strchr(part, '/');
        if (slash != NULL) { *slash = '\0'; }
        if (part[0] == '\0' || part[0] == '.' || strchr(part, '\\') != NULL)
        {
            (void)close(directory);
            return -1;
        }
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (slash != NULL) { flags |= O_DIRECTORY; }
        int next = openat(directory, part, flags);
        (void)close(directory);
        if (next < 0 || slash == NULL) { return next; }
        directory = next;
        part = slash + 1;
    }
    (void)close(directory);
    return -1;
}

int static_file_read(const char *url_path, char *out, size_t cap, size_t *len, const char **content_type)
{
    assert(url_path != NULL);
    assert(out != NULL);
    assert(len != NULL);
    assert(content_type != NULL);

    if (static_files_enabled() == 0) { return -1; }

    const char *rest = NULL;
    if (strip_prefix(url_path, &rest) != 0) { return -1; }

    size_t      rest_len = strlen(rest);
    const char *suffix   = "";

    if (rest_len == 0u)
    {
        rest   = "/";
        suffix = "index.html";
    }
    else if (rest[rest_len - 1u] == '/')
    {
        suffix = "index.html";
    }

    char candidate[STATIC_MAX_PATH];
    int  n = snprintf(candidate, sizeof(candidate), "%s%s", rest[0] == '/' ? rest + 1 : rest, suffix);
    if ((n <= 0) || ((size_t)n >= sizeof(candidate))) { return -1; }

    const char *mime = content_type_for(candidate);
    int descriptor = open_static_path(candidate);
    if (descriptor < 0) { return -1; }
    FILE *file = fdopen(descriptor, "rb");
    if (file == NULL)
    {
        (void)close(descriptor);
        return -1;
    }

    struct stat info;
    if ((fstat(fileno(file), &info) != 0) || (!S_ISREG(info.st_mode)) || (info.st_size < 0) || ((size_t)info.st_size > cap) || ((size_t)info.st_size > (size_t)STATIC_MAX_BYTES))
    {
        (void)fclose(file);
        return -1;
    }

    size_t want = (size_t)info.st_size;
    size_t got  = fread(out, 1u, want, file);
    (void)fclose(file);

    if (got != want) { return -1; }

    *len          = got;
    *content_type = mime;

    return 0;
}
