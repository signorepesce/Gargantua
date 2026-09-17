#include "sqlite_internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static sqlite3_stmt *schema_query(sqlite3 *db, const char *sql, const char *name)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        return NULL;
    }
    if (sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK)
    {
        (void)sqlite3_finalize(st);
        return NULL;
    }
    return st;
}

static int schema_error(const TypeInfo *type, const char *field, const char *reason)
{
    (void)fprintf(stderr, "gargantua: schema %s.%s: %s\n", type->name, field, reason);
    return -1;
}

static int affinity_matches(const char *declared, FieldKind kind)
{
    if (declared == NULL || strlen(declared) >= 128u)
    {
        return 0;
    }
    char upper[128];
    size_t n = strlen(declared);
    for (size_t i = 0u; i <= n; i++)
    {
        upper[i] = (char)toupper((unsigned char)declared[i]);
    }
    int integer = strstr(upper, "INT") != NULL;
    int text = !integer && (strstr(upper, "CHAR") || strstr(upper, "CLOB") || strstr(upper, "TEXT"));
    int real = !integer && !text && (strstr(upper, "REAL") || strstr(upper, "FLOA") || strstr(upper, "DOUB"));
    if (kind == FIELD_STR)
    {
        return text;
    }
    if (kind == FIELD_DOUBLE)
    {
        return real;
    }
    if (kind == FIELD_BOOL)
    {
        return integer || strcmp(upper, "BOOLEAN") == 0;
    }
    return integer;
}

static int check_columns(sqlite3 *db, const TypeInfo *type)
{
    sqlite3_stmt *st =
        schema_query(db, "SELECT name,type,\"notnull\",pk,dflt_value,hidden FROM pragma_table_xinfo(?)", type->name);
    if (st == NULL)
    {
        return schema_error(type, "*", "cannot inspect columns");
    }
    unsigned seen[64] = {0};
    int result = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
    {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *declared = (const char *)sqlite3_column_text(st, 1);
        int required = sqlite3_column_int(st, 2);
        int primary = sqlite3_column_int(st, 3);
        int hidden = sqlite3_column_int(st, 5);
        int slot = -1;
        if (name == NULL)
        {
            result = -1;
            break;
        }
        for (unsigned i = 0u; i < type->field_count; i++)
        {
            if (strcmp(name, type->fields[i].name) == 0)
            {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0)
        {
            if (primary || (required && !hidden && sqlite3_column_type(st, 4) == SQLITE_NULL))
            {
                result = schema_error(type, name, "extra required column has no default");
            }
            continue;
        }
        const FieldInfo *field = &type->fields[slot];
        seen[slot]++;
        int expected_pk = (field->flags & FIELD_PRIMARY_KEY) != 0u;
        int expected_required =
            (field->flags & FIELD_NOT_NULL) || (field->kind != FIELD_STR && !(field->flags & FIELD_NULLABLE));
        if (hidden || !affinity_matches(declared, field->kind) || (primary != 0) != expected_pk ||
            (expected_pk && (primary != 1 || declared == NULL || strcasecmp(declared, "INTEGER") != 0)))
        {
            result = schema_error(type, name, "incompatible type, primary key or generated column");
        }
        else if (!primary && (required != 0) != (expected_required != 0))
        {
            result = schema_error(type, name, "nullability differs; use $nullable(T) or migrate the column");
        }
    }
    if (rc != SQLITE_DONE)
    {
        result = -1;
    }
    if (sqlite3_finalize(st) != SQLITE_OK)
    {
        result = -1;
    }
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        if (seen[i] != 1u)
        {
            result = schema_error(type, type->fields[i].name, "column missing");
        }
    }
    return result;
}

static int check_constraints(sqlite3 *db, const TypeInfo *type)
{
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *field = &type->fields[i];
        if ((field->flags & FIELD_UNIQUE) && !(field->flags & FIELD_PRIMARY_KEY))
        {
            sqlite3_stmt *st =
                schema_query(db,
                             "SELECT x.name FROM pragma_index_list(?) AS l JOIN pragma_index_info(l.name) AS x "
                             "WHERE l.\"unique\"=1 AND l.partial=0 "
                             "AND (SELECT count(*) FROM pragma_index_info(l.name))=1",
                             type->name);
            if (st == NULL)
            {
                return -1;
            }
            int found = 0, rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            {
                const char *name = (const char *)sqlite3_column_text(st, 0);
                if (name && strcmp(name, field->name) == 0)
                {
                    found = 1;
                }
            }
            int finished = sqlite3_finalize(st);
            if (!found || rc != SQLITE_DONE || finished != SQLITE_OK)
            {
                return schema_error(type, field->name, "single-column UNIQUE constraint missing");
            }
        }
        if (field->references != NULL)
        {
            sqlite3_stmt *st = schema_query(db,
                                            "SELECT \"table\",\"from\",\"to\","
                                            "(SELECT count(*) FROM pragma_foreign_key_list(?) AS b WHERE b.id=a.id) "
                                            "FROM pragma_foreign_key_list(?) AS a",
                                            type->name);
            if (st == NULL)
            {
                return -1;
            }
            if (sqlite3_bind_text(st, 2, type->name, -1, SQLITE_TRANSIENT) != SQLITE_OK)
            {
                (void)sqlite3_finalize(st);
                return -1;
            }
            int found = 0, rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            {
                const char *target = (const char *)sqlite3_column_text(st, 0);
                const char *from = (const char *)sqlite3_column_text(st, 1);
                const char *to = (const char *)sqlite3_column_text(st, 2);
                if (target && from && to && !strcmp(target, field->references) && !strcmp(from, field->name) &&
                    !strcmp(to, field->reference_key) && sqlite3_column_int(st, 3) == 1)
                {
                    found = 1;
                }
            }
            int finished = sqlite3_finalize(st);
            if (!found || rc != SQLITE_DONE || finished != SQLITE_OK)
            {
                return schema_error(type, field->name, "foreign key constraint missing or incompatible");
            }
        }
    }
    return 0;
}

int sqlite_schema_check(sqlite3 *db, const TypeInfo *type)
{
    sqlite3_stmt *st = schema_query(db, "SELECT type FROM sqlite_schema WHERE name=? AND type='table'", type->name);
    if (st == NULL)
    {
        return -1;
    }
    int exists = sqlite3_step(st) == SQLITE_ROW;
    int done = sqlite3_finalize(st);
    if (!exists || done != SQLITE_OK)
    {
        return schema_error(type, "*", "table missing");
    }
    st = schema_query(db, "SELECT count(*) FROM pragma_index_list(?) WHERE origin='pk'", type->name);
    if (st == NULL)
    {
        return -1;
    }
    int rowid_key = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0;
    done = sqlite3_finalize(st);
    if (!rowid_key || done != SQLITE_OK)
    {
        return schema_error(type, "*", "$id requires an INTEGER rowid primary key");
    }
    if (check_columns(db, type) != 0)
    {
        return -1;
    }
    return check_constraints(db, type);
}
