#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "dynbuf.h"
#include "test.h"
#include <string.h>

int main(void)
{
    DynBuf b;

    CHECK(dynbuf_init(&b, 1024u * 1024u) == 0);
    CHECK(b.data == NULL);
    CHECK(b.cap == 0u);
    CHECK(dynbuf_init(&b, 0u) == -1);

    CHECK(dynbuf_init(&b, 1024u * 1024u) == 0);
    CHECK(dynbuf_reserve(&b, 10u) == 0);
    CHECK(b.cap >= 10u);
    CHECK(b.data != NULL);

    memcpy(b.data, "hello world", 11u);
    b.len = 11u;

    CHECK(dynbuf_reserve(&b, 400u * 1024u) == 0);
    CHECK(b.cap >= 400u * 1024u);
    CHECK(memcmp(b.data, "hello world", 11u) == 0);

    dynbuf_consume(&b, 6u);
    CHECK(b.len == 5u);
    CHECK(memcmp(b.data, "world", 5u) == 0);

    CHECK(dynbuf_reserve(&b, 4u * 1024u * 1024u) == -1);
    CHECK(b.truncated == 1);

    dynbuf_release(&b);
    CHECK(b.data == NULL);
    CHECK(b.len == 0u && b.cap == 0u);

    dynbuf_free(&b);
    TEST_REPORT("dynbuf");
}
