#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

#define ARENA_CANARY_SIZE 16
#define ARENA_MIN_SIZE    4096u
#define ARENA_KEEP_MAX    (64u * 1024u)
#define ARENA_MMAP_MIN    (256u * 1024u)

typedef struct ArenaChunk
{
    struct ArenaChunk *next;
    unsigned char     *base;
    size_t             cap;
    size_t             used;
    size_t             bytes;
    int                mapped;
} ArenaChunk;

typedef struct
{
    ArenaChunk   *head;
    ArenaChunk   *spare;
    size_t        used;
    size_t        max;
    size_t        peak;
    unsigned long refusals;
} Arena;

int arena_init(Arena *a, size_t max);
void *arena_alloc(Arena *a, size_t n);
int arena_reset(Arena *a);
void arena_free(Arena *a);
size_t arena_available(const Arena *a);

#endif
