#ifndef GARGANTUA_QUERY_H
#define GARGANTUA_QUERY_H

#define DB_MAX_ARGS 64
#define DB_MAX_SQL 8192

typedef struct
{
    FieldKind kind;
    int i;
    long l;
    double d;
    str s;
} SqlArg;

#define SQL_INT(v) ((SqlArg){FIELD_INT, (v), 0L, 0.0, ""})
#define SQL_LONG(v) ((SqlArg){FIELD_LONG, 0, (v), 0.0, ""})
#define SQL_DOUBLE(v) ((SqlArg){FIELD_DOUBLE, 0, 0L, (v), ""})
#define SQL_BOOL(v) ((SqlArg){FIELD_BOOL, ((v) ? 1 : 0), 0L, 0.0, ""})
#define SQL_TEXT(v) ((SqlArg){FIELD_STR, 0, 0L, 0.0, (v)})
#define SQL_ARGS(...) ((const SqlArg[]){__VA_ARGS__}), ((int)(sizeof((const SqlArg[]){__VA_ARGS__}) / sizeof(SqlArg)))
#define SQL_NULL() ((SqlArg){FIELD_KIND_COUNT, 0, 0L, 0.0, NULL})
SqlArg sql_option_int(Nullable_int value);
SqlArg sql_option_long(Nullable_long value);
SqlArg sql_option_double(Nullable_double value);
SqlArg sql_option_bool(Nullable_bool value);
#define SQL_OPTION_int(v) sql_option_int(v)
#define SQL_OPTION_long(v) sql_option_long(v)
#define SQL_OPTION_double(v) sql_option_double(v)
#define SQL_OPTION_bool(v) sql_option_bool(v)
#define SQL_NOARGS ((const SqlArg *)0), 0

#define DB_FILTER_MAX 8

typedef struct
{
    str field;
    str op;
    SqlArg value;
} DbFilter;

typedef struct
{
    DbFilter filters[DB_FILTER_MAX];
    int count;
    str order;
    bool descending;
    int page;
    int size;
} DbQuery;

SqlArg sql_value_int(int value);
SqlArg sql_value_long(long value);
SqlArg sql_value_double(double value);
SqlArg sql_value_bool(bool value);
SqlArg sql_value_text(str value);
#define $sql_value(v)                                                                                                  \
    _Generic((v),                                                                                                      \
        int: sql_value_int,                                                                                            \
        long: sql_value_long,                                                                                          \
        double: sql_value_double,                                                                                      \
        bool: sql_value_bool,                                                                                          \
        char *: sql_value_text,                                                                                        \
        const char *: sql_value_text)(v)
#define $filter(field, op, value) ((DbFilter){#field, (op), $sql_value(value)})
#define $filter_null(field) ((DbFilter){#field, "IS NULL", SQL_NULL()})
#define $filter_not_null(field) ((DbFilter){#field, "IS NOT NULL", SQL_NULL()})
#define $query(...) ((DbQuery){__VA_ARGS__})

#endif
