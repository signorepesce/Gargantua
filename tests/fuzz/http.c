#include "http.h"
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
    HttpRequest request = {0};
    (void)http_parse_request(input, size, &request);
    free(input);
    return 0;
}
