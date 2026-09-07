#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "generator.h"
#include <assert.h>
#include <stdio.h>

static char g_src[GENERATOR_MAX_SOURCE + 1];

char *source_file_read(const char *path, size_t *out_len)
{
    assert(path != NULL);
    assert(out_len != NULL);

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        (void)fprintf(stderr, "gargantua: could not open %s\n", path);
        return NULL;
    }

    size_t n = fread(g_src, 1u, (size_t)GENERATOR_MAX_SOURCE + 1u, f);

    int too_big = (n > (size_t)GENERATOR_MAX_SOURCE) ? 1 : 0;
    int failed  = (ferror(f) != 0) ? 1 : 0;

    if (fclose(f) != 0)
    {
        (void)fprintf(stderr, "gargantua: could not close %s\n", path);
        return NULL;
    }

    if (failed == 1)
    {
        (void)fprintf(stderr, "gargantua: read error in %s\n", path);
        return NULL;
    }

    if (too_big == 1)
    {
        (void)fprintf(stderr, "gargantua: %s exceeds %d bytes\n", path, GENERATOR_MAX_SOURCE);
        return NULL;
    }

    assert(n <= (size_t)GENERATOR_MAX_SOURCE);
    g_src[n] = '\0';
    *out_len = n;
    return g_src;
}
