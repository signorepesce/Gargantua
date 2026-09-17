#include "generator.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536u) { return 0; }
    char *input = malloc(size + 1u);
    if (input == NULL) { return 0; }
    memcpy(input, data, size);
    input[size] = '\0';
    static Generator generator;
    generator_init(&generator);
    if (generator_begin_file(&generator, "fuzz.c") == 0)
    {
        generator_parse_source(&generator, input, size);
        generator_validate_model(&generator);
    }
    free(input);
    return 0;
}
