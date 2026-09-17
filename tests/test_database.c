#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "framework_internal.h"
#include "arena.h"
#include "test.h"
#include <limits.h>
#include <string.h>

typedef struct
{
    int id;
    str name;
    Nullable_int amount;
} Sample;

static const FieldInfo fields[] = {
    {"id", FIELD_INT, (unsigned short)offsetof(Sample, id), sizeof(int), FIELD_PRIMARY_KEY, NULL, NULL, NULL, {0}},
    {"name",
     FIELD_STR,
     (unsigned short)offsetof(Sample, name),
     sizeof(str),
     FIELD_NOT_NULL | FIELD_UNIQUE,
     NULL,
     NULL,
     NULL,
     {0}},
    {"amount",
     FIELD_INT,
     (unsigned short)offsetof(Sample, amount),
     sizeof(Nullable_int),
     FIELD_NULLABLE,
     NULL,
     NULL,
     NULL,
     {0}}};
static const TypeInfo sample_type = {"Sample", sizeof(Sample), 3, fields};

int main(void)
{
    Arena arena;
    CHECK(arena_init(&arena, 1024u * 1024u) == 0);
    arena_bind(&arena);
    CHECK(db_open(":memory:") == 0);
    CHECK(db_create_table(&sample_type) == 0);
    CHECK(db_check_schema(&sample_type) == 0);
    CHECK(db_begin() == 0);
    CHECK(db_execute("INSERT INTO Sample(name,amount) VALUES (?,?)", SQL_ARGS(SQL_TEXT("first"), SQL_NULL())) == 1);
    CHECK(db_begin() == 0);
    CHECK(db_execute("INSERT INTO Sample(name,amount) VALUES (?,?)", SQL_ARGS(SQL_TEXT("nested"), SQL_INT(0))) == 1);
    CHECK(db_rollback() == 0);
    CHECK(db_commit() == 0);
    CHECK(db_depth() == 0);
    Sample row = {0};
    CHECK(db_query_one(&sample_type, &row, "SELECT * FROM Sample WHERE name=?", SQL_ARGS(SQL_TEXT("nested"))) == 0);
    CHECK(db_query_one(&sample_type, &row, "SELECT * FROM Sample WHERE name=?", SQL_ARGS(SQL_TEXT("first"))) == 1);
    CHECK(!row.amount.has_value);
    char json[256];
    CHECK(json_write_struct(&sample_type, &row, json, sizeof(json)) == 0);
    CHECK(strstr(json, "\"amount\":null") != NULL);
    row.amount = $some(int, 0);
    CHECK(json_write_struct(&sample_type, &row, json, sizeof(json)) == 0);
    CHECK(strstr(json, "\"amount\":0") != NULL);
    DbQuery query = $query(.filters = {$filter_null(amount)}, .count = 1);
    CHECK(db_search(&sample_type, query).count == 1);
    query.filters[0] = $filter(name, "LIKE", "fir%");
    CHECK(db_search(&sample_type, query).count == 1);
    query.filters[0].op = "= 1; DROP TABLE Sample";
    CHECK(db_search(&sample_type, query).count == 0 && request_failed());
    request_fail_reset();
    query.count = DB_FILTER_MAX + 1;
    CHECK(db_search(&sample_type, query).count == 0 && request_failed());
    request_fail_reset();
    query.count = -1;
    CHECK(db_search(&sample_type, query).count == 0 && request_failed());
    request_fail_reset();
    query.count = 0;
    query.page = INT_MAX;
    query.size = 100;
    CHECK(db_search(&sample_type, query).count == 0 && request_failed());
    request_fail_reset();
    query.page = 0;
    query.order = "name\";DROP TABLE Sample";
    CHECK(db_search(&sample_type, query).count == 0 && request_failed());
    request_fail_reset();
    CHECK(db_check_schema(&sample_type) == 0);
    CHECK(db_execute("UPDATE Sample SET amount=NULL", SQL_NOARGS) == 1);
    CHECK(db_execute("SELECT ?", SQL_NOARGS) == -1);
    CHECK(db_execute("SELECT 1; SELECT 2", SQL_NOARGS) == -1);
    CHECK(db_begin() == 0);
    CHECK(db_execute("INSERT INTO Sample(name) VALUES (?)", SQL_ARGS(SQL_TEXT("first"))) == -1);
    CHECK(db_commit() == -1);
    CHECK(db_rollback() == 0);
    CHECK(db_depth() == 0);
    CHECK(db_ready() == 1);
    db_close();
    arena_bind(NULL);
    arena_free(&arena);
    TEST_REPORT("database");
}
