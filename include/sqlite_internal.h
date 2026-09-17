#ifndef GARGANTUA_SQLITE_INTERNAL_H
#define GARGANTUA_SQLITE_INTERNAL_H
#include "db.h"
#include <sqlite3.h>
int sqlite_schema_check(sqlite3 *db, const TypeInfo *type);
#endif
