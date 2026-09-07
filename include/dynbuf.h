#ifndef DYNBUF_H
#define DYNBUF_H

#include <stddef.h>

#define DYNBUF_MIN_CHUNK 8192u
#define DYNBUF_MMAP_MIN  (256u * 1024u)

typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
    size_t max;
    size_t bytes;
    int    mapped;
    int    truncated;
} DynBuf;

int dynbuf_init(DynBuf *b, size_t max);
int dynbuf_reserve(DynBuf *b, size_t need);
void dynbuf_consume(DynBuf *b, size_t n);
void dynbuf_release(DynBuf *b);
void dynbuf_free(DynBuf *b);

#endif
