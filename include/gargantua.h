#ifndef GARGANTUA_H
#define GARGANTUA_H

#include <stdbool.h>
#include <stddef.h>
#include "response.h"
#include "auth.h"

typedef const char *str;

#define $table(T) typedef struct T T; struct T
#define $json(T)  typedef struct T T; struct T
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
    FIELD_PRIMARY_KEY      = 1u,
    FIELD_NOT_NULL = 2u,
    FIELD_UNIQUE  = 4u
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

enum { RULE_MIN = 1u, RULE_MAX = 2u, RULE_SIZE = 4u, RULE_EMAIL = 8u };

typedef struct TypeInfo TypeInfo;

typedef struct
{
    const char    *name;
    FieldKind         kind;
    unsigned short offset;
    unsigned short size;
    unsigned       flags;
    const TypeInfo  *nested;
    const char    *references;
    const char    *reference_key;
    FieldRules        rules;
} FieldInfo;

struct TypeInfo
{
    const char     *name;
    unsigned short  size;
    unsigned short  field_count;
    const FieldInfo  *fields;
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

void *request_alloc(size_t size);
int row_list_write(RowList list, int paginated, char *out, size_t cap);
RowList row_list_make(const TypeInfo *type, const void *items, size_t count);
#define $items(T, ...) row_list_make(&T##__type, (T[]){ __VA_ARGS__ }, \
    sizeof((T[]){ __VA_ARGS__ }) / sizeof(T))

const char *field_kind_name(FieldKind kind);
int field_text(const TypeInfo *type, const void *row, const char *name, char *out, size_t cap);

int json_write_struct(const TypeInfo *type, const void *obj, char *out, size_t cap);

int create_table_sql_write(const TypeInfo *type, char *out, size_t cap);

int validate_struct(const TypeInfo *type, const void *obj, const char **bad);

int text_write(str text, char *out, size_t cap);

#define PATH_PARAM_MAX 8
#define QUERY_PARAM_MAX  12
#define PARAM_VALUE_LEN  64

typedef struct
{

    char path_value[PATH_PARAM_MAX][PARAM_VALUE_LEN];
    int  path_count;

    char query_name[QUERY_PARAM_MAX][PARAM_VALUE_LEN];
    char query_value[QUERY_PARAM_MAX][PARAM_VALUE_LEN];
    int  query_count;
    int  invalid;
} RequestParams;

int path_param_int(const RequestParams *p, int index, int *ok);
str path_param_text(const RequestParams *p, int index);

int query_parse(RequestParams *p, char *query);

str query_text(const RequestParams *p, str name);

int query_int(const RequestParams *p, str name, int *ok);
int query_has(const RequestParams *p, str name);

str request_header(str name);

void request_bind(const void *request);

void arena_bind(void *arena);

str arena_intern(str text);

#define FORMAT_MAX 1024

str str_format(str format, ...) __attribute__((format(printf, 1, 2)));
str str_concat(str a, str b);

str  time_now(void);
void log_write(const char *level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#define $now()     time_now()
#define $log(...)  log_write("info", __VA_ARGS__)
#define $warn(...) log_write("warn", __VA_ARGS__)

int text_equals(str a, str b);
int text_empty(str a);

#define $eq(a, b)  (text_equals((a), (b)) == 1)
#define $empty(a)  (text_empty((a)) == 1)

#define $header(name) request_header(name)

#define FAIL_MESSAGE_LEN 128
#define ERROR_CAP 16384
#define ERROR_FIELDS_MAX 16
int error_json_write(int status, str message, char *out, size_t cap);
void field_error_add(str field, str code, str message);
void request_fail_code(int status, str code, str message);
int json_read_mode(const TypeInfo *type, void *obj, char *json, size_t len, int partial);
int patch_apply(const TypeInfo *type, void *target, const void *changes);
int body_has_key(str path);
int body_is_null(str path);
#define $has(field) body_has_key(#field)
#define $is_null(field) body_is_null(#field)

void request_fail(int status, str message);

void request_fail_field(str field);
int  request_failed(void);
int  request_fail_status(void);
str  request_fail_message(void);
void request_fail_reset(void);

int request_active(void);
void request_raise(int status, str message);
#define THROW_VOID(status, message) \
    do { request_raise((status), (message)); return; } while (0)
#define THROW_VALUE(status, message, value) \
    do { request_raise((status), (message)); return (value); } while (0)
#define THROW_SELECT(_1, _2, _3, NAME, ...) NAME
#define $throw(...) \
    THROW_SELECT(__VA_ARGS__, THROW_VALUE, THROW_VOID, \
                    THROW_INVALID_ARITY)(__VA_ARGS__)

#define $fail(...)   RENAMED__USE_$throw_status_message_value
#define $abort(...)  RENAMED__USE_$throw_status_message_value
#define $return(...) RENAMED__USE_$throw_status_message_value

typedef struct
{
    int active;
    int level;
} Transaction;

Transaction transaction_begin(void);
int  transaction_live(Transaction *t);
void transaction_end(Transaction *t);
void transaction_cleanup(Transaction *t);
void transaction_abort_if_open(void);
int  transaction_depth(void);

#define JOIN_INNER(a, b) a##b
#define JOIN_KIND(a, b) JOIN_INNER(a, b)
#define TRANSACTION_SCOPE(name) \
    for (Transaction name __attribute__((cleanup(transaction_cleanup))) = transaction_begin(); \
         transaction_live(&name); transaction_end(&name))
#define $transaction TRANSACTION_SCOPE(JOIN_KIND(wrap_tx_, __COUNTER__))

typedef int (*DispatchFn)(const RequestParams *, char *, size_t, char *, size_t);
int dispatch_task(void (*handler)(void));
int dispatch_route(DispatchFn handler, const RequestParams *params, char *body, size_t len, char *out, size_t cap);

int url_decode(char *text);

#define JSON_MAX_TOKENS 256

int json_read_struct(const TypeInfo *type, void *obj, char *json, size_t len);

#endif
