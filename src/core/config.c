#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "config.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 512
#define CONFIG_ENV_MAX  96

typedef struct
{
    char key[CONFIG_KEY_LEN];
    char value[CONFIG_VALUE_LEN];
} Entry;

static Entry g_entries[CONFIG_MAX_KEYS];
static int   g_count;

static char *trim_spaces(char *text)
{
    assert(text != NULL);
    assert(CONFIG_LINE_MAX > 0);

    char *start = text;
    int   guard = 0;

    while ((*start != '\0') && (guard < CONFIG_LINE_MAX))
    {
        if (isspace((unsigned char)*start) == 0) { break; }
        start++;
        guard++;
    }

    size_t len = strlen(start);
    guard = 0;
    while ((len > 0u) && (guard < CONFIG_LINE_MAX))
    {
        if (isspace((unsigned char)start[len - 1u]) == 0) { break; }
        len--;
        start[len] = '\0';
        guard++;
    }

    return start;
}

static int store_entry(const char *key, const char *value)
{
    assert(key != NULL);
    assert(value != NULL);
    if (*key == '\0' || strlen(key) >= CONFIG_KEY_LEN || strlen(value) >= CONFIG_VALUE_LEN) { return -1; }

    for (int i = 0; (i < g_count) && (i < CONFIG_MAX_KEYS); i++)
    {
        if (strcmp(g_entries[i].key, key) == 0) { return -1; }
    }

    if (g_count >= CONFIG_MAX_KEYS) { return -1; }

    (void)snprintf(g_entries[g_count].key, CONFIG_KEY_LEN, "%s", key);
    (void)snprintf(g_entries[g_count].value, CONFIG_VALUE_LEN, "%s", value);
    g_count++;
    return 0;
}

int config_load(const char *path)
{
    assert(path != NULL);
    assert(CONFIG_MAX_KEYS > 0);

    const char *override = getenv("GARGANTUA_CONFIG");
    if ((override != NULL) && (override[0] != '\0')) { path = override; }

    g_count = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) { return errno == ENOENT ? 0 : -1; }

    char line[CONFIG_LINE_MAX];
    int  lineno = 0;
    int  bad    = 0;

    while ((fgets(line, (int)sizeof(line), f) != NULL) && (lineno < CONFIG_MAX_KEYS * 8))
    {
        lineno++;
        size_t line_len = strlen(line);
        if (line_len == sizeof(line) - 1u && line[line_len - 1u] != '\n')
        {
            bad++;
            break;
        }

        char *hash = strchr(line, '#');
        if (hash != NULL) { *hash = '\0'; }

        char *text = trim_spaces(line);
        if (text[0] == '\0') { continue; }

        char *eq = strchr(text, '=');
        if (eq == NULL)
        {
            (void)fprintf(stderr, "%s:%d: expected 'key = value'\n", path, lineno);
            bad++;
            continue;
        }

        *eq = '\0';
        if (store_entry(trim_spaces(text), trim_spaces(eq + 1)) != 0) { bad++; }
    }

    if (ferror(f) || (lineno >= CONFIG_MAX_KEYS * 8 && !feof(f))) { bad++; }
    if (fclose(f) != 0) { bad++; }
    if (bad) { g_count = 0; return -1; }
    return g_count;
}

static void env_name(const char *key, char *out, size_t cap)
{
    assert(key != NULL);
    assert(out != NULL);

    size_t n = 0u;
    while ((key[n] != '\0') && ((n + 1u) < cap))
    {
        char c = key[n];
        out[n] = (c == '.') ? '_' : (char)toupper((unsigned char)c);
        n++;
    }
    out[n] = '\0';
}

const char *config_str(const char *key, const char *fallback)
{
    assert(key != NULL);
    assert(fallback != NULL);

    char name[CONFIG_ENV_MAX];
    env_name(key, name, sizeof(name));

    const char *from_env = getenv(name);
    if ((from_env != NULL) && (from_env[0] != '\0')) { return from_env; }

    for (int i = 0; (i < g_count) && (i < CONFIG_MAX_KEYS); i++)
    {
        if (strcmp(g_entries[i].key, key) == 0) { return g_entries[i].value; }
    }

    return fallback;
}

int config_int(const char *key, int fallback)
{
    assert(key != NULL);
    assert(CONFIG_VALUE_LEN > 0);

    const char *text = config_str(key, "");
    if (text[0] == '\0') { return fallback; }

    char *end   = NULL;
    errno = 0;
    long  value = strtol(text, &end, 10);

    if (errno == ERANGE || (end == text) || (*end != '\0') || (value < -2147483647L) || (value > 2147483647L))
    {
        (void)fprintf(stderr, "gargantua: '%s' is not a number in '%s'," " using %d\n", text, key, fallback);
        return fallback;
    }

    return (int)value;
}
