#ifndef TEST_H
#define TEST_H

#include <stdio.h>

static int test_checks;
static int test_failures;

#define CHECK(cond) do { \
    test_checks++; \
    if (!(cond)) { \
        test_failures++; \
        (void)fprintf(stderr, "    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define TEST_REPORT(name) do { \
    (void)printf("  %-12s %3d checks, %d failed\n", (name), test_checks, test_failures); \
    return (test_failures == 0) ? 0 : 1; \
} while (0)

#endif
