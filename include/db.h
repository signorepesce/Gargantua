#ifndef DB_H
#define DB_H

#include "gargantua.h"

#define DB_MAX_ARGS 64
#define DB_MAX_SQL  8192

typedef struct
{
    FieldKind kind;
    int    i;
    long   l;
    double d;
    str    s;
} SqlArg;

#define SQL_INT(v)      ((SqlArg){ FIELD_INT, (v), 0L, 0.0, "" })
#define SQL_LONG(v)      ((SqlArg){ FIELD_LONG, 0, (v), 0.0, "" })
#define SQL_DOUBLE(v)      ((SqlArg){ FIELD_DOUBLE, 0, 0L, (v), "" })
#define SQL_BOOL(v)      ((SqlArg){ FIELD_BOOL, ((v) ? 1 : 0), 0L, 0.0, "" })
#define SQL_TEXT(v)      ((SqlArg){ FIELD_STR, 0, 0L, 0.0, (v) })
#define SQL_ARGS(...) ((const SqlArg[]){ __VA_ARGS__ }), \
                     ((int)(sizeof((const SqlArg[]){ __VA_ARGS__ }) / sizeof(SqlArg)))
#define SQL_NOARGS    ((const SqlArg *)0), 0

RowList db_query_page(const TypeInfo *type, str key, str filter, int id, int page, int size);

str db_name(void);

int db_open(str url);

void db_close(void);

int db_execute(str sql, const SqlArg *args, int nargs);

int db_insert_id(str sql, const SqlArg *args, int nargs);

int db_create_table(const TypeInfo *type);

int db_last_id(void);

int db_script(const char *sql);
int db_migration_count(void);
int db_migration(int version, const char *name, const char *sql, int apply);
int db_limits(int busy_ms, int query_ms);
int db_ready(void);
int db_depth(void);
void db_abort_all(void);
int db_begin(void);
int db_commit(void);
int db_rollback(void);

int db_query_one(const TypeInfo *type, void *out, str sql, const SqlArg *args, int nargs);

int db_query_many(const TypeInfo *type, void *rows, int max, str sql, const SqlArg *args, int nargs);

#endif
