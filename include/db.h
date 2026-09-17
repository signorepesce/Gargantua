#ifndef DB_H
#define DB_H

#include "gargantua.h"

RowList db_search(const TypeInfo *type, DbQuery query);
int db_check_schema(const TypeInfo *type);

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
