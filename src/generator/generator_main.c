#include "generator.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ARG_MAX_COUNT 1024

typedef struct
{
    const char *files[GENERATOR_MAX_FILES];
    int         file_count;
    const char *out_source;
    const char *out_header;
    int         with_main;
} Options;

static void print_usage(void)
{
    (void)fprintf(stderr, "usage: gargantua <source.c> [source2.c ...]" " -o <output.c> [-h <header.h>] [-m]\n");
}

static int parse_args(int argc, char **argv, Options *opt)
{
    assert(argv != NULL);
    assert(opt != NULL);

    memset(opt, 0, sizeof(*opt));

    if ((argc < 2) || (argc > ARG_MAX_COUNT)) { return -1; }

    for (int i = 1; (i < argc) && (i < ARG_MAX_COUNT); i++)
    {
        int has_value = ((i + 1) < argc) ? 1 : 0;

        if (word_equals(argv[i], "-o") == 1)
        {
            if (has_value == 0) { return -1; }
            opt->out_source = argv[i + 1];
            i++;
        }
        else if (word_equals(argv[i], "-m") == 1)
        {
            opt->with_main = 1;
        }
        else if (word_equals(argv[i], "-h") == 1)
        {
            if (has_value == 0) { return -1; }
            opt->out_header = argv[i + 1];
            i++;
        }
        else if (argv[i][0] == '-')
        {
            return -1;
        }
        else
        {
            if (opt->file_count >= GENERATOR_MAX_FILES) { return -1; }
            opt->files[opt->file_count] = argv[i];
            opt->file_count++;
        }
    }

    return ((opt->out_source != NULL) && (opt->file_count > 0)) ? 0 : -1;
}

static int run_generator(const Options *opt)
{
    assert(opt != NULL);
    assert(opt->file_count > 0);

    static Generator ctx;
    generator_init(&ctx);
    ctx.want_main = opt->with_main;

    for (int i = 0; (i < opt->file_count) && (i < GENERATOR_MAX_FILES); i++)
    {
        size_t len = 0u;
        char  *src = source_file_read(opt->files[i], &len);
        if (src == NULL) { return 1; }
        if (generator_begin_file(&ctx, opt->files[i]) != 0) { return 1; }
        generator_parse_source(&ctx, src, len);
    }

    generator_validate_model(&ctx);

    if (ctx.errors > 0)
    {
        (void)fprintf(stderr, "gargantua: %d error(s), nothing generated\n", ctx.errors);
        return 1;
    }

    if (opt->out_source != NULL)
    {
        if (generator_write_source(&ctx, opt->out_source) != 0) { return 1; }
    }

    if (opt->out_header != NULL)
    {
        if (generator_write_header(&ctx, opt->out_header) != 0) { return 1; }
    }

    (void)fprintf(stderr, "gargantua: %d type(s), %d transactional service(s)," " %d route(s)" " from %d file(s)\n", ctx.type_count, ctx.service_count, ctx.route_count, ctx.file_count);
    return 0;
}

int main(int argc, char **argv)
{
    Options opt;

    if (parse_args(argc, argv, &opt) != 0)
    {
        print_usage();
        return 2;
    }

    return run_generator(&opt);
}
