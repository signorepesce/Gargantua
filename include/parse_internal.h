#ifndef GARGANTUA_PARSE_INTERNAL_H
#define GARGANTUA_PARSE_INTERNAL_H

#include "generator.h"

typedef struct
{
    int      lineno;
    int      in_struct;
    int      need_open;
    int      in_block_comment;
    unsigned pending;
    ParsedField rules;
    char pending_references[GENERATOR_MAX_NAME];
    ParsedType *current_type;
    int      route_armed;
    int      pending_transactional;
    int      pending_auth;
    int      pending_public;
    char     pending_role[GENERATOR_MAX_NAME];
    int      brace_depth;
    int      task_armed;
    long     pending_interval_ms;
    char     pending_produces[GENERATOR_MAX_CONTENT_TYPE];
    int      transaction_block_armed;
    int      transaction_depth;
    char     route_method[GENERATOR_MAX_METHOD];
    char     route_url[GENERATOR_MAX_URL];
} ParseState;

int count_url_params(const char *url);

int arm_route_annotation(Generator *ctx, ParseState *st, const char *line);

void drop_dangling_auth(Generator *ctx, ParseState *st);

int extract_function_name(const char *line, char *out, size_t cap);

int extract_params(Generator *ctx, ParseState *st, const char *line, ParsedRoute *r, int service);

int extract_return_type(const char *line, char *out, size_t cap);

void guard_transaction(Generator *ctx, ParseState *st, const char *line);

int is_safe_identifier(const char *name);

int line_only_annotations(const char *line, unsigned *flags);

void open_type(Generator *ctx, ParseState *st, const char *line);

int parse_auth_annotation(Generator *ctx, ParseState *st, const char *line);

int parse_field(Generator *ctx, char *line, int lineno, ParsedField *out);

int parse_produces_annotation(Generator *ctx, ParseState *st, const char *line);

int parse_repeat_annotation(Generator *ctx, ParseState *st, const char *line);

void parse_route(Generator *ctx, ParseState *st, const char *line);

int parse_route_url(Generator *ctx, ParseState *st, const char *line, const char *word, char out[GENERATOR_MAX_URL]);

void parse_service(Generator *ctx, ParseState *st, const char *line);

int parse_store_annotation(Generator *ctx, ParseState *st, const char *line);

int parse_table_name(Generator *ctx, int line_no, const char *line, char *out, size_t cap);

void parse_task(Generator *ctx, ParseState *st, const char *line);

int parse_transactional_annotation(Generator *ctx, ParseState *st, const char *line);

int report_removed_annotation(Generator *ctx, ParseState *st, const char *line);

void strip_comments(char *line, int *in_block);

int validate_route_url(Generator *ctx, ParseState *st, const char *url);

#endif
