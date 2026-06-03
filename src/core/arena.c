#include "arena.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define ARENA_ALIGN (sizeof(long double) > sizeof(void *) \
                     ? sizeof(long double) : sizeof(void *))

static const unsigned char CANARY[ARENA_CANARY_SIZE] = {
    0xA5u, 0x5Au, 0xC3u, 0x3Cu, 0xA5u, 0x5Au, 0xC3u, 0x3Cu,
    0xA5u, 0x5Au, 0xC3u, 0x3Cu, 0xA5u, 0x5Au, 0xC3u, 0x3Cu
};

static void secure_zero(void *p, size_t n)
{
    assert(p != NULL);
    assert(n > 0u);

    volatile unsigned char *vp = p;
    for (size_t i = 0u; i < n; i++) { vp[i] = 0u; }
}

static void canary_write(Arena *a)
{
    assert(a != NULL);
    assert(a->base != NULL);

    memcpy(a->base + a->cap, CANARY, (size_t)ARENA_CANARY_SIZE);
}

static int canary_ok(const Arena *a)
{
    assert(a != NULL);
    assert(a->base != NULL);

    return (memcmp(a->base + a->cap, CANARY, (size_t)ARENA_CANARY_SIZE) == 0) ? 1 : 0;
}

int arena_init(Arena *a, void *backing, size_t size)
{
    if ((a == NULL) || (backing == NULL)) { return -1; }
    if (size < (size_t)(ARENA_MIN_SIZE + ARENA_CANARY_SIZE)) { return -1; }

    a->base     = backing;
    a->cap      = size - (size_t)ARENA_CANARY_SIZE;
    a->used     = 0u;
    a->peak     = 0u;
    a->refusals = 0uL;

    assert(a->cap >= (size_t)ARENA_MIN_SIZE);
    assert(a->used == 0u);

    secure_zero(a->base, a->cap);
    canary_write(a);
    return 0;
}

void *arena_alloc(Arena *a, size_t n)
{

    assert(a != NULL);
    assert(a->base != NULL);

    if (n == 0u) { return NULL; }

    const size_t align = ARENA_ALIGN;
    if (n > (SIZE_MAX - (align - 1u)))
    {
        a->refusals++;
        return NULL;
    }
    size_t want = (n + (align - 1u)) & ~(align - 1u);

    assert(a->cap >= a->used);
    if (want > (a->cap - a->used))
    {
        a->refusals++;
        return NULL;
    }

    unsigned char *p = a->base + a->used;
    a->used += want;
    if (a->used > a->peak) { a->peak = a->used; }

    assert(a->used <= a->cap);
    return p;
}

int arena_reset(Arena *a)
{
    assert(a != NULL);
    assert(a->base != NULL);

    int intact = canary_ok(a);

    if (a->used > 0u) { secure_zero(a->base, a->used); }
    a->used = 0u;

    if (intact == 0)
    {

        canary_write(a);
        return -1;
    }
    return 0;
}

size_t arena_available(const Arena *a)
{
    assert(a != NULL);
    assert(a->cap >= a->used);

    return a->cap - a->used;
}
