#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "dynbuf.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = p;
    for (size_t i = 0u; i < n; i++) { vp[i] = 0u; }
}

int dynbuf_init(DynBuf *b, size_t max)
{
    assert(b != NULL);
    if (max == 0u) { return -1; }
    b->data = NULL;
    b->len = 0u;
    b->cap = 0u;
    b->max = max;
    b->bytes = 0u;
    b->mapped = 0;
    b->truncated = 0;
    return 0;
}

int dynbuf_reserve(DynBuf *b, size_t need)
{
    assert(b != NULL);
    if (need > b->max) { b->truncated = 1; return -1; }
    if (need <= b->cap) { return 0; }

    size_t newcap = (b->cap != 0u) ? b->cap : (size_t)DYNBUF_MIN_CHUNK;
    while (newcap < need)
    {
        if (newcap > (b->max / 2u)) { newcap = b->max; break; }
        newcap *= 2u;
    }
    if (newcap < need) { newcap = need; }

    if (newcap >= (size_t)DYNBUF_MMAP_MIN)
    {
        void *m = mmap(NULL, newcap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { return -1; }
        if (b->len > 0u) { memcpy(m, b->data, b->len); }
        if (b->data != NULL)
        {
            secure_zero(b->data, b->len);
            if (b->mapped != 0) { (void)munmap(b->data, b->bytes); }
            else { free(b->data); }
        }
        b->data = m;
        b->cap = newcap;
        b->bytes = newcap;
        b->mapped = 1;
        return 0;
    }

    char *p = realloc(b->data, newcap);
    if (p == NULL) { return -1; }
    b->data = p;
    b->cap = newcap;
    b->bytes = newcap;
    b->mapped = 0;
    return 0;
}

void dynbuf_consume(DynBuf *b, size_t n)
{
    assert(b != NULL);
    assert(n <= b->len);
    size_t left = b->len - n;
    if (left > 0u) { memmove(b->data, b->data + n, left); }
    b->len = left;
}

void dynbuf_release(DynBuf *b)
{
    assert(b != NULL);
    if (b->data != NULL)
    {
        secure_zero(b->data, b->cap);
        if (b->mapped != 0) { (void)munmap(b->data, b->bytes); }
        else { free(b->data); }
        b->data = NULL;
    }
    b->len = 0u;
    b->cap = 0u;
    b->bytes = 0u;
    b->mapped = 0;
    b->truncated = 0;
}

void dynbuf_free(DynBuf *b)
{
    dynbuf_release(b);
}
