#ifndef JSON_STRUCT_H
#define JSON_STRUCT_H

#include <stddef.h>

#define JSON_PATH_SEGMENT 128
#define JSON_MAX_DEPTH 128
#define JSON_MAX_OBJECT_KEYS 256
#define JSON_MAX_KEY_BYTES 128

typedef enum
{
    JSON_UNDEF = 0,
    JSON_OBJ,
    JSON_ARR,
    JSON_STR,
    JSON_PRIM
} JsonType;

typedef struct
{
    JsonType type;
    int start;
    int end;
    int parent;
} JsonToken;

int json_parse(const char *js, size_t len, JsonToken *toks, int max);

void json_copy_str(const char *js, const JsonToken *t, char *out, size_t outsz);

int json_object_get(const char *js, const JsonToken *toks, int ntok, int obj, const char *key);

int json_path_get(const char *js, const JsonToken *toks, int ntok, int root, const char *path);

long json_token_long(const char *js, const JsonToken *t);

int json_token_long_checked(const char *js, const JsonToken *t, long *out);
int json_token_double_checked(const char *js, const JsonToken *t, double *out);
int json_token_bool_checked(const char *js, const JsonToken *t, int *out);

#endif
