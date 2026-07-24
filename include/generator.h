#ifndef GENERATOR_H
#define GENERATOR_H

#include <stddef.h>
#include <stdio.h>

#include "scheduler.h"

#define GENERATOR_MAX_SOURCE     (1024 * 1024)
#define GENERATOR_MAX_LINE    512
#define GENERATOR_MAX_LINES   65536
#define GENERATOR_MAX_TYPES   256
#define GENERATOR_MAX_FIELDS  64
#define GENERATOR_MAX_NAME    64
#define GENERATOR_MAX_TOKENS  8
#define GENERATOR_MAX_PATH    512
#define GENERATOR_MAX_ROUTES  512
#define GENERATOR_MAX_SERVICES 256
#define GENERATOR_MAX_METHOD  8
#define GENERATOR_MAX_URL     128
#define GENERATOR_MAX_PARAMS  8
#define GENERATOR_MAX_FILES  512
#define GENERATOR_MAX_ROUTE_SEGMENTS 16
#define GENERATOR_MAX_CONTENT_TYPE 64
#define GENERATOR_MAX_STORES 16

typedef struct
{
    char     name[GENERATOR_MAX_NAME];
    char     c_type[GENERATOR_MAX_NAME];
    char     kind[GENERATOR_MAX_NAME];
    unsigned flags;
    char references[GENERATOR_MAX_NAME];
    char reference_key[GENERATOR_MAX_NAME];
    unsigned validation;
    long double min;
    long double max;
    unsigned size_min;
    unsigned size_max;
    char min_arg[GENERATOR_MAX_NAME];
    char max_arg[GENERATOR_MAX_NAME];
    long min_int;
    long max_int;
} ParsedField;

typedef struct
{
    char     name[GENERATOR_MAX_NAME];
    ParsedField fields[GENERATOR_MAX_FIELDS];
    int      field_count;
    int      line;
    int      file_index;
    int      json_only;
} ParsedType;

typedef struct
{
    char type[GENERATOR_MAX_NAME];
    char name[GENERATOR_MAX_NAME];
    int  is_body;
    int  is_query;
    int  path_slot;
} ParsedParam;

typedef struct
{
    char     method[GENERATOR_MAX_METHOD];
    char     url[GENERATOR_MAX_URL];
    char     function_name[GENERATOR_MAX_NAME];
    char     return_type[GENERATOR_MAX_NAME];
    ParsedParam params[GENERATOR_MAX_PARAMS];
    int      param_count;
    int      line;
    int      file_index;
    int      transactional;
    int      collection;
    int      authenticated;
    int      public_route;
    char     role[GENERATOR_MAX_NAME];
    char     produces[GENERATOR_MAX_CONTENT_TYPE];
} ParsedRoute;

typedef struct
{
    char function_name[GENERATOR_MAX_NAME];
    long interval_ms;
    int  line;
    int  file_index;
} ParsedTask;

typedef struct
{
    char type[GENERATOR_MAX_NAME];
    int  capacity;
    int  line;
    int  file_index;
} ParsedStore;

typedef struct
{
    char     function_name[GENERATOR_MAX_NAME];
    char     return_type[GENERATOR_MAX_NAME];
    ParsedParam params[GENERATOR_MAX_PARAMS];
    int      param_count;
    int      line;
    int      file_index;
} ParsedService;

typedef struct
{
    char path[GENERATOR_MAX_PATH];
    int  has_table;
    int  has_route;
    int  has_service;
    int  has_task;
} SourceFile;

typedef struct
{

    char     path[GENERATOR_MAX_PATH];

    SourceFile files[GENERATOR_MAX_FILES];
    int      file_count;

    ParsedType  types[GENERATOR_MAX_TYPES];
    int      type_count;
    ParsedRoute routes[GENERATOR_MAX_ROUTES];
    int      route_count;
    ParsedService services[GENERATOR_MAX_SERVICES];
    int        service_count;
    ParsedTask tasks[SCHEDULER_MAX_TASKS];
    int        task_count;
    ParsedStore stores[GENERATOR_MAX_STORES];
    int         store_count;
    int      errors;
    int      want_main;
} Generator;

char *source_file_read(const char *path, size_t *out_len);

void  line_strip_comment(char *line);
char *line_trim(char *line);
int   line_split_words(char *line, char *tok[], int max);
int   word_equals(const char *a, const char *b);
const char *field_kind_from_c_type(const char *c_type);
unsigned    field_flag_from_word(const char *word);
const char *path_file_name(const char *path);
const char *http_method_from_word(const char *word);
int line_quoted_text(const char *line, char *out, size_t cap);

void generator_init(Generator *ctx);

int  generator_begin_file(Generator *ctx, const char *path);

void generator_error(Generator *ctx, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void generator_parse_source(Generator *ctx, char *src, size_t len);
void generator_validate_model(Generator *ctx);

int generator_write_openapi(FILE *out, const Generator *ctx);
int generator_write_source(const Generator *ctx, const char *out_path);
int generator_write_header(const Generator *ctx, const char *out_path);

#endif
