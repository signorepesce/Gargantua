#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "arena.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_ALIGN (sizeof(long double) > sizeof(void *) \
                     ? sizeof(long double) : sizeof(void *))

static const unsigned char CANARY[ARENA_CANARY_SIZE] = {
    0xA5u, 0x5Au, 0xC3u, 0x3Cu, 0xA5u, 0x5Au, 0xC3u, 0x3Cu,
    0xA5u, 0x5Au, 0xC3u, 0x3Cu, 0xA5u, 0x5Au, 0xC3u, 0x3Cu
};

static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *vp = p;
    for (size_t i = 0u; i < n; i++) { vp[i] = 0u; }
}

static ArenaChunk *chunk_new(size_t cap)
{
    size_t total = sizeof(ArenaChunk) + cap + (size_t)ARENA_CANARY_SIZE;
    if (total < cap) { return NULL; }

    ArenaChunk *c = NULL;
    int mapped = 0;

    if (total >= (size_t)ARENA_MMAP_MIN)
    {
        void *m = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { return NULL; }
        c = m;
        mapped = 1;
    }
    else
    {
        c = malloc(total);
        if (c == NULL) { return NULL; }
    }

    c->next   = NULL;
    c->base   = (unsigned char *)c + sizeof(ArenaChunk);
    c->cap    = cap;
    c->used   = 0u;
    c->bytes  = total;
    c->mapped = mapped;
    memcpy(c->base + cap, CANARY, (size_t)ARENA_CANARY_SIZE);
    return c;
}

static void chunk_release(ArenaChunk *c)
{
    if (c->mapped != 0) { (void)munmap(c, c->bytes); }
    else { free(c); }
}

static int chunk_ok(const ArenaChunk *c)
{
    return (memcmp(c->base + c->cap, CANARY, (size_t)ARENA_CANARY_SIZE) == 0) ? 1 : 0;
}

int arena_init(Arena *a, size_t max)
{
    if ((a == NULL) || (max < ARENA_MIN_SIZE)) { return -1; }

    a->head     = NULL;
    a->spare    = NULL;
    a->used     = 0u;
    a->max      = max;
    a->peak     = 0u;
    a->refusals = 0uL;
    return 0;
}

void *arena_alloc(Arena *a, size_t n)
{
    assert(a != NULL);

    if ((n == 0u) || (a->max == 0u)) { return NULL; }

    const size_t align = ARENA_ALIGN;
    if (n > (SIZE_MAX - (align - 1u))) { a->refusals++; return NULL; }
    size_t want = (n + (align - 1u)) & ~(align - 1u);

    if (want > (a->max - a->used)) { a->refusals++; return NULL; }

    if ((a->head != NULL) && (want <= (a->head->cap - a->head->used)))
    {
        unsigned char *p = a->head->base + a->head->used;
        a->head->used += want;
        a->used += want;
        if (a->used > a->peak) { a->peak = a->used; }
        return p;
    }

    size_t cap = ARENA_MIN_SIZE;
    while (cap < want)
    {
        if (cap > (a->max / 2u)) { cap = want; break; }
        cap *= 2u;
    }
    if (cap < want) { cap = want; }

    ArenaChunk *c = NULL;
    if ((a->spare != NULL) && (a->spare->cap >= want))
    {
        c = a->spare;
        a->spare = NULL;
        c->used = 0u;
    }
    else
    {
        c = chunk_new(cap);
        if (c == NULL) { a->refusals++; return NULL; }
    }

    c->next = a->head;
    a->head = c;

    unsigned char *p = c->base + c->used;
    c->used += want;
    a->used += want;
    if (a->used > a->peak) { a->peak = a->used; }
    return p;
}

int arena_reset(Arena *a)
{
    assert(a != NULL);

    int intact = 1;
    ArenaChunk *c = a->head;
    while (c != NULL)
    {
        ArenaChunk *next = c->next;
        if (chunk_ok(c) == 0) { intact = 0; }
        if (c->used > 0u) { secure_zero(c->base, c->used); }

        if ((a->spare == NULL) && (c->cap <= ARENA_KEEP_MAX))
        {
            c->used = 0u;
            c->next = NULL;
            a->spare = c;
        }
        else
        {
            chunk_release(c);
        }
        c = next;
    }

    a->head = NULL;
    a->used = 0u;
    return (intact != 0) ? 0 : -1;
}

void arena_free(Arena *a)
{
    assert(a != NULL);

    (void)arena_reset(a);
    if (a->spare != NULL) { chunk_release(a->spare); a->spare = NULL; }
    a->max = 0u;
}

size_t arena_available(const Arena *a)
{
    assert(a != NULL);
    assert(a->max >= a->used);

    return a->max - a->used;
}
