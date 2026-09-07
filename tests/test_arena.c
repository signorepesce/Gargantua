#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "arena.h"
#include "test.h"
#include <string.h>

int main(void)
{
    Arena a;

    CHECK(arena_init(&a, 1024u * 1024u) == 0);
    CHECK(arena_available(&a) == 1024u * 1024u);
    CHECK(arena_alloc(&a, 0u) == NULL);

    /* basic bump */
    unsigned char *p = arena_alloc(&a, 100u);
    CHECK(p != NULL);
    memset(p, 0xAB, 100u);
    CHECK(p[0] == 0xAB && p[99] == 0xAB);

    /* alignment */
    void *q = arena_alloc(&a, 1u);
    CHECK(q != NULL);
    CHECK(((size_t)q % sizeof(void *)) == 0u);

    /* POINTER STABILITY: a later big allocation must not move earlier ones */
    unsigned char *first = arena_alloc(&a, 32u);
    CHECK(first != NULL);
    memcpy(first, "stable-across-growth", 21u);
    for (int i = 0; i < 40; i++) { CHECK(arena_alloc(&a, 8192u) != NULL); }
    CHECK(memcmp(first, "stable-across-growth", 21u) == 0);

    /* ceiling is enforced, and refusal is counted */
    CHECK(arena_alloc(&a, 64u * 1024u * 1024u) == NULL);
    CHECK(a.refusals > 0uL);

    /* reset frees and rewinds */
    CHECK(arena_reset(&a) == 0);
    CHECK(arena_available(&a) == 1024u * 1024u);
    CHECK(arena_alloc(&a, 100u) != NULL);

    /* peak survives reset */
    CHECK(a.peak > 0u);

    arena_free(&a);
    CHECK(a.max == 0u);

    /* too small a ceiling is rejected */
    Arena b;
    CHECK(arena_init(&b, 8u) == -1);

    TEST_REPORT("arena");
}
