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

