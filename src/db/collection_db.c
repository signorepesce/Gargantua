#include "db.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int is_safe_identifier(str name)
{
    if (name == NULL || name[0] == '\0') { return 0; }
    for (size_t i = 0u; i < 64u; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c == 0u) { return 1; }
        if (isalpha(c) == 0 && c != '_' && (i == 0u || isdigit(c) == 0)) { return 0; }
    }
    return 0;
}

RowList db_query_page(const TypeInfo *type, str key, str filter, int id, int page, int size)
{
    RowList result = { type, NULL, 0, page, size, 0 };
    if (page < 0 || size < 1 || size > PAGE_MAX || page > INT_MAX / size)
    {
        request_fail(400, "page must be nonnegative; size must be 1..100");
        return result;
    }
    if (type == NULL || type->size == 0u || is_safe_identifier(type->name) == 0 || is_safe_identifier(key) == 0 || (filter != NULL && is_safe_identifier(filter) == 0))
    {
        request_fail(500, "invalid collection descriptor");
        return result;
    }
    size_t count = (size_t)size + 1u;
    if (count > SIZE_MAX / type->size)
    {
        request_fail(500, "collection allocation overflow");
        return result;
    }
    result.items = request_alloc(count * type->size);
    if (result.items == NULL) { return result; }
    char sql[512];
    int n;
    if (filter == NULL)
    {
        n = snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\" ORDER BY \"%s\" LIMIT ? OFFSET ?", type->name, key);
    }
    else
    {
        n = snprintf(sql, sizeof(sql), "SELECT * FROM \"%s\" WHERE \"%s\" = ? ORDER BY \"%s\" LIMIT ? OFFSET ?", type->name, filter, key);
    }
    if (n < 0 || (size_t)n >= sizeof(sql))
    {
        request_fail(500, "collection SQL overflow");
        return result;
    }
    int found = filter == NULL
        ? db_query_many(type, result.items, size + 1, sql, SQL_ARGS(SQL_INT(size + 1), SQL_INT(page * size)))
        : db_query_many(type, result.items, size + 1, sql, SQL_ARGS(SQL_INT(id), SQL_INT(size + 1), SQL_INT(page * size)));
    if (found < 0 || found > size + 1)
    {
        request_fail(500, "collection query failed");
        return result;
    }
    result.has_more = found > size;
    result.count = found > size ? size : found;
    return result;
}
