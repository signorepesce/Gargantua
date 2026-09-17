#include "framework_internal.h"
#include "db.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int name_safe(str name)
{
    if (name == NULL || name[0] == '\0')
    {
        return 0;
    }
    for (size_t i = 0u; i < 64u; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c == 0u)
        {
            return 1;
        }
        if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (i > 0u && c >= '0' && c <= '9')))
        {
            return 0;
        }
    }
    return 0;
}

static const FieldInfo *find_field(const TypeInfo *type, str name)
{
    if (!name_safe(name))
    {
        return NULL;
    }
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        if (strcmp(type->fields[i].name, name) == 0)
        {
            return &type->fields[i];
        }
    }
    return NULL;
}

static int append_sql(char *sql, size_t *used, const char *format, ...) __attribute__((format(printf, 3, 4)));

static int append_sql(char *sql, size_t *used, const char *format, ...)
{
    if (*used >= DB_MAX_SQL)
    {
        return -1;
    }
    va_list args;
    va_start(args, format);
    int n = vsnprintf(sql + *used, DB_MAX_SQL - *used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= DB_MAX_SQL - *used)
    {
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

static int filter_valid(const FieldInfo *field, const DbFilter *filter)
{
    if (field == NULL || filter->op == NULL)
    {
        return 0;
    }
    if (strcmp(filter->op, "IS NULL") == 0 || strcmp(filter->op, "IS NOT NULL") == 0)
    {
        return filter->value.kind == FIELD_KIND_COUNT;
    }
    int equal = strcmp(filter->op, "=") == 0 || strcmp(filter->op, "!=") == 0;
    int ordered = strcmp(filter->op, "<") == 0 || strcmp(filter->op, "<=") == 0 || strcmp(filter->op, ">") == 0 ||
                  strcmp(filter->op, ">=") == 0;
    int like = strcmp(filter->op, "LIKE") == 0;
    if (!equal && !ordered && !like)
    {
        return 0;
    }
    if (like && field->kind != FIELD_STR)
    {
        return 0;
    }
    int bool_literal =
        field->kind == FIELD_BOOL && filter->value.kind == FIELD_INT && (filter->value.i == 0 || filter->value.i == 1);
    if (filter->value.kind != field->kind && !bool_literal)
    {
        return 0;
    }
    if (field->kind == FIELD_DOUBLE && !isfinite(filter->value.d))
    {
        return 0;
    }
    if (field->kind == FIELD_STR && filter->value.s == NULL)
    {
        return 0;
    }
    return field->kind >= FIELD_INT && field->kind <= FIELD_STR;
}

RowList db_search(const TypeInfo *type, DbQuery query)
{
    int size = query.size == 0 ? 20 : query.size;
    RowList result = {type, NULL, 0, query.page, size, 0};
    if (type == NULL || type->fields == NULL || type->field_count == 0u || type->field_count > 64u ||
        type->size == 0u || !name_safe(type->name) || query.count < 0 || query.count > DB_FILTER_MAX ||
        query.page < 0 || size < 1 || size > PAGE_MAX || query.page > INT_MAX / size)
    {
        request_fail(400, "invalid query or pagination");
        return result;
    }
    const FieldInfo *key = NULL;
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        if (type->fields[i].flags & FIELD_PRIMARY_KEY)
        {
            key = &type->fields[i];
        }
    }
    const FieldInfo *order = query.order ? find_field(type, query.order) : key;
    if (key == NULL || order == NULL || !name_safe(key->name) || !name_safe(order->name))
    {
        request_fail(400, "unknown sort field");
        return result;
    }
    char sql[DB_MAX_SQL];
    size_t used = 0u;
    if (append_sql(sql, &used, "SELECT * FROM \"%s\"", type->name) != 0)
    {
        return result;
    }
    SqlArg args[DB_FILTER_MAX + 2];
    int count = 0;
    for (int i = 0; i < query.count; i++)
    {
        const DbFilter *filter = &query.filters[i];
        const FieldInfo *field = find_field(type, filter->field);
        if (!filter_valid(field, filter))
        {
            request_fail(400, "invalid filter field, operator or value type");
            return result;
        }
        int is_null = filter->value.kind == FIELD_KIND_COUNT;
        if (append_sql(sql, &used, "%s\"%s\" %s%s", i ? " AND " : " WHERE ", field->name, filter->op,
                       is_null ? "" : " ?") != 0)
        {
            request_fail(500, "query exceeds SQL limit");
            return result;
        }
        if (!is_null)
        {
            args[count++] = filter->value;
        }
    }
    if (append_sql(sql, &used, " ORDER BY \"%s\" %s, \"%s\" ASC LIMIT ? OFFSET ?", order->name,
                   query.descending ? "DESC" : "ASC", key->name) != 0)
    {
        request_fail(500, "query exceeds SQL limit");
        return result;
    }
    args[count++] = SQL_INT(size + 1);
    args[count++] = SQL_INT(query.page * size);
    result.items = request_alloc(((size_t)size + 1u) * type->size);
    if (result.items == NULL)
    {
        return result;
    }
    int found = db_query_many(type, result.items, size + 1, sql, args, count);
    if (found < 0)
    {
        request_fail(500, "query failed");
        return result;
    }
    result.count = found > size ? size : found;
    result.has_more = found > size;
    return result;
}
