#include <gargantua.h>

static int helper(void)
{
    return 1;
}

int increment(int value)
{
    return value + helper();
}
