#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

#define ARENA_CANARY_SIZE 16
#define ARENA_MIN_SIZE    4096

typedef struct
{
    unsigned char *base;
    size_t         cap;
    size_t         used;
    size_t         peak;
    unsigned long  refusals;
} Arena;

int arena_init(Arena *a, void *backing, size_t size);

void *arena_alloc(Arena *a, size_t n);

int arena_reset(Arena *a);

size_t arena_available(const Arena *a);

#endif
