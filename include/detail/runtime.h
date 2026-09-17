#ifndef GARGANTUA_DETAIL_RUNTIME_H
#define GARGANTUA_DETAIL_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include "response.h"
#include "auth.h"

typedef const char *str;

#define GARGANTUA_NULLABLE_TYPE(T)                                                                                     \
    typedef struct                                                                                                     \
    {                                                                                                                  \
        bool has_value;                                                                                                \
        T value;                                                                                                       \
    } Nullable_##T
GARGANTUA_NULLABLE_TYPE(int);
GARGANTUA_NULLABLE_TYPE(long);
GARGANTUA_NULLABLE_TYPE(double);
GARGANTUA_NULLABLE_TYPE(bool);
#undef GARGANTUA_NULLABLE_TYPE
#define $nullable(T) Nullable_##T
#define $some(T, v) ((Nullable_##T){true, (v)})
#define $null(T) ((Nullable_##T){0})

#define $table(T)                                                                                                      \
    typedef struct T T;                                                                                                \
    struct T
#define $json(T)                                                                                                       \
    typedef struct T T;                                                                                                \
    struct T
#define $references(T)
#define $list(T) RowList
#define $page(T) RowList

#define $id
#define $not_null
#define $min(value)
#define $max(value)
#define $size(min, max)
#define $email
#define $unique

#define $transactional

#define $store(T, count)
#define $produces(type)
#define $repeat(interval)
#define $on_start

#define $get(path)
#define $post(path)
#define $put(path)
#define $patch(path)
#define $delete(path)

typedef enum
{
    FIELD_INT = 0,
    FIELD_LONG,
    FIELD_DOUBLE,
    FIELD_BOOL,
    FIELD_STR,
    FIELD_OBJECT,
    FIELD_KIND_COUNT
} FieldKind;

enum
{
    FIELD_PRIMARY_KEY = 1u,
    FIELD_NOT_NULL = 2u,
    FIELD_UNIQUE = 4u,
    FIELD_NULLABLE = 8u
};

typedef struct
{
    unsigned flags;
    long double min;
    long double max;
    unsigned size_min;
    unsigned size_max;
    long min_int;
    long max_int;
} FieldRules;

enum
{
    RULE_MIN = 1u,
    RULE_MAX = 2u,
    RULE_SIZE = 4u,
    RULE_EMAIL = 8u
};

typedef struct TypeInfo TypeInfo;

typedef struct
{
    const char *name;
    FieldKind kind;
    unsigned short offset;
    unsigned short size;
    unsigned flags;
    const TypeInfo *nested;
    const char *references;
    const char *reference_key;
    FieldRules rules;
} FieldInfo;

struct TypeInfo
{
    const char *name;
    unsigned short size;
    unsigned short field_count;
    const FieldInfo *fields;
};

#define PAGE_MAX 100
#define NEST_DEPTH_MAX 8

typedef struct
{
    const TypeInfo *type;
    void *items;
    int count;
    int page;
    int size;
    int has_more;
} RowList;

RowList row_list_make(const TypeInfo *type, const void *items, size_t count);
#define $items(T, ...) row_list_make(&T##__type, (T[]){__VA_ARGS__}, sizeof((T[]){__VA_ARGS__}) / sizeof(T))

#define PATH_PARAM_MAX 8
#define QUERY_PARAM_MAX 12
#define PARAM_VALUE_LEN 64

typedef struct
{

    char path_value[PATH_PARAM_MAX][PARAM_VALUE_LEN];
    int path_count;

    char query_name[QUERY_PARAM_MAX][PARAM_VALUE_LEN];
    char query_value[QUERY_PARAM_MAX][PARAM_VALUE_LEN];
    int query_count;
    int invalid;
} RequestParams;

str request_header(str name);

str arena_intern(str text);

#define FORMAT_MAX 1024

str str_format(str format, ...) __attribute__((format(printf, 1, 2)));
str str_concat(str a, str b);

str time_now(void);
void log_write(const char *level, const char *format, ...) __attribute__((format(printf, 2, 3)));

#define $now() time_now()
#define $log(...) log_write("info", __VA_ARGS__)
#define $warn(...) log_write("warn", __VA_ARGS__)

int text_equals(str a, str b);
int text_empty(str a);

#define $eq(a, b) (text_equals((a), (b)) == 1)
#define $empty(a) (text_empty((a)) == 1)

#define $header(name) request_header(name)

#define FAIL_MESSAGE_LEN 128
#define ERROR_CAP 16384
#define ERROR_FIELDS_MAX 16
int body_has_key(str path);
int body_is_null(str path);
#define $has(field) body_has_key(#field)
#define $is_null(field) body_is_null(#field)

void request_raise(int status, str message);
#define THROW_VOID(status, message)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        request_raise((status), (message));                                                                            \
        return;                                                                                                        \
    } while (0)
#define THROW_VALUE(status, message, value)                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        request_raise((status), (message));                                                                            \
        return (value);                                                                                                \
    } while (0)
#define THROW_SELECT(_1, _2, _3, NAME, ...) NAME
#define $throw(...) THROW_SELECT(__VA_ARGS__, THROW_VALUE, THROW_VOID, THROW_INVALID_ARITY)(__VA_ARGS__)

typedef struct
{
    int active;
    int level;
} Transaction;

Transaction transaction_begin(void);
int transaction_live(Transaction *t);
void transaction_end(Transaction *t);
void transaction_cleanup(Transaction *t);

#define JOIN_INNER(a, b) a##b
#define JOIN_KIND(a, b) JOIN_INNER(a, b)
#define TRANSACTION_SCOPE(name)                                                                                        \
    for (Transaction name __attribute__((cleanup(transaction_cleanup))) = transaction_begin();                         \
         transaction_live(&name); transaction_end(&name))
#define $transaction TRANSACTION_SCOPE(JOIN_KIND(wrap_tx_, __COUNTER__))

typedef int (*DispatchFn)(const RequestParams *, char *, size_t, char *, size_t);

#define JSON_MAX_TOKENS 256

#include "detail/query.h"

#endif
