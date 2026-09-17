#include "json.h"
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536u) { return 0; }
    JsonToken tokens[256];
    int count = json_parse((const char *)data, size, tokens, 256);
    for (int i = 0; i < count; i++)
    {
        char text[64];
        json_copy_str((const char *)data, &tokens[i], text, sizeof(text));
        long number;
        double decimal;
        int boolean;
        (void)json_token_long_checked((const char *)data, &tokens[i], &number);
        (void)json_token_double_checked((const char *)data, &tokens[i], &decimal);
        (void)json_token_bool_checked((const char *)data, &tokens[i], &boolean);
    }
    return 0;
}
