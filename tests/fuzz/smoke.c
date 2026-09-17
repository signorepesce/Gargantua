#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint32_t rng = 42u;
static uint32_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

int main(int argc, char **argv)
{
    if (argc < 2) { return 2; }
    DIR *directory = opendir(argv[1]);
    if (directory == NULL) { return 2; }
    struct dirent *entry;
    unsigned runs = 0u;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] == '.') { continue; }
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", argv[1], entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { return 2; }
        FILE *file = fopen(path, "rb");
        if (file == NULL) { return 2; }
        uint8_t seed[4096], input[4096];
        size_t length = fread(seed, 1u, sizeof(seed), file);
        (void)fclose(file);
        (void)LLVMFuzzerTestOneInput(seed, length);
        for (unsigned i = 0u; i < 5000u; i++)
        {
            memcpy(input, seed, length);
            size_t size = length;
            unsigned changes = 1u + next_random() % 8u;
            for (unsigned change = 0u; change < changes; change++)
            {
                size_t at = next_random() % (size + 1u);
                if (at < sizeof(input))
                {
                    input[at] = (uint8_t)next_random();
                    if (at == size) { size++; }
                }
            }
            if (i % 4u == 0u) { size = next_random() % (size + 1u); }
            (void)LLVMFuzzerTestOneInput(input, size);
            runs++;
        }
    }
    (void)closedir(directory);
    (void)fprintf(stderr, "Mutation smoke: %u inputs passed (libFuzzer unavailable)\n", runs);
    return 0;
}
