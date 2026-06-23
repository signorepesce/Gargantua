#include "db.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <strings.h>

#define SQLITE_MAX_ROWS 4096

static sqlite3 *g_db;
static pthread_mutex_t g_db_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_last_id;

static _Thread_local int t_depth;
static _Thread_local int t_control;
static _Thread_local int t_failed_level;
static _Thread_local struct timespec t_deadline;
static int g_busy_ms = 1000;
static int g_query_ms = 5000;

static void db_failure(void)
{
    if (!request_active()) { return; }
    int code = g_db ? sqlite3_extended_errcode(g_db) : SQLITE_ERROR;
    if (code == SQLITE_CONSTRAINT_UNIQUE || code == SQLITE_CONSTRAINT_PRIMARYKEY)
    { request_fail_code(409, "unique_conflict", "a record with this value already exists"); }
    else if (code == SQLITE_CONSTRAINT_FOREIGNKEY)
    { request_fail_code(409, "reference_conflict", "operation conflicts with a related record"); }
    else if ((code & 255) == SQLITE_CONSTRAINT)
    { request_fail_code(409, "constraint_conflict", "operation violates a database constraint"); }
    else if ((code & 255) == SQLITE_BUSY || (code & 255) == SQLITE_LOCKED || (code & 255) == SQLITE_INTERRUPT || (code & 255) == SQLITE_FULL)
    { request_fail_code(503, "database_unavailable", "database temporarily unavailable"); }
    else { request_fail_code(500, "database_error", "database operation failed"); }
}

static long long now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return -1; }
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

static int db_lock(void)
{
    if (t_depth > 0) { return 0; }
    long long start = now_ms();
    if (start < 0) { return -1; }
    for (int i = 0; i <= g_busy_ms; i++)
    {
        int rc = pthread_mutex_trylock(&g_db_lock);
        if (rc == 0) { return 0; }
        if (rc != EBUSY || now_ms() - start >= g_busy_ms)
        {
            if (request_active()) { request_fail_code(503, "database_unavailable", "database temporarily unavailable"); }
            return -1;
        }
        struct timespec pause = { 0, 1000000L };
        (void)nanosleep(&pause, NULL);
    }
    return -1;
}

static void db_unlock(void)
{
    if (t_depth == 0) { (void)pthread_mutex_unlock(&g_db_lock); }
}

static int sqlite_progress_handler(void *unused)
{
    (void)unused;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { return 1; }
    return now.tv_sec > t_deadline.tv_sec ||
        (now.tv_sec == t_deadline.tv_sec && now.tv_nsec >= t_deadline.tv_nsec);
}

static int sqlite_authorizer(void *unused, int action, const char *a, const char *b, const char *db, const char *source)
{
    (void)unused; (void)b; (void)db; (void)source;
    if ((action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT) && !t_control) { return SQLITE_DENY; }
    if (action == SQLITE_ATTACH || action == SQLITE_DETACH) { return SQLITE_DENY; }
    if (action == SQLITE_PRAGMA && a != NULL && (strcasecmp(a, "foreign_keys") == 0 || strcasecmp(a, "writable_schema") == 0 || strcasecmp(a, "journal_mode") == 0)) { return SQLITE_DENY; }
    return SQLITE_OK;
}

int db_limits(int busy_ms, int query_ms)
{
    if (g_db != NULL || busy_ms < 1 || busy_ms > 60000 || query_ms < 1 || query_ms > 60000) { return -1; }
    g_busy_ms = busy_ms;
    g_query_ms = query_ms;
    return 0;
}

static void statement_deadline(void)
{
    (void)clock_gettime(CLOCK_MONOTONIC, &t_deadline);
    t_deadline.tv_sec += g_query_ms / 1000;
    t_deadline.tv_nsec += (long)(g_query_ms % 1000) * 1000000L;
    if (t_deadline.tv_nsec >= 1000000000L)
    {
        t_deadline.tv_sec++;
        t_deadline.tv_nsec -= 1000000000L;
    }
}

str db_name(void)
{
    assert(DB_MAX_ARGS > 0);
    assert(DB_MAX_SQL > 0);

    return "sqlite";
}

int db_open(str url)
{
    assert(url != NULL);

    if (db_lock() != 0) { return -1; }
    if ((g_db != NULL) || (url[0] == '\0'))
    {
        db_unlock();
        return -1;
    }
    int rc = sqlite3_open_v2(url, &g_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK)
    {
        (void)fprintf(stderr, "gargantua: sqlite '%s': %s\n", url, (g_db != NULL) ? sqlite3_errmsg(g_db) : "unknown");
        if (g_db != NULL) { (void)sqlite3_close(g_db); }
        g_db = NULL;
        db_unlock();
        return -1;
    }

    (void)sqlite3_extended_result_codes(g_db, 1);
    (void)sqlite3_busy_timeout(g_db, g_busy_ms);
    if (sqlite3_exec(g_db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL) != SQLITE_OK)
    {
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; }
        db_failure();
        (void)fprintf(stderr, "gargantua: sqlite operation failed (%d)\n", sqlite3_extended_errcode(g_db));
        (void)sqlite3_close(g_db);
        g_db = NULL;
        db_unlock();
        return -1;
    }
    sqlite3_stmt *pragma = NULL;
    int enabled = 0;
    if ((sqlite3_prepare_v2(g_db, "PRAGMA foreign_keys", -1, &pragma, NULL) == SQLITE_OK) && (sqlite3_step(pragma) == SQLITE_ROW)) { enabled = sqlite3_column_int(pragma, 0) == 1; }
    if (sqlite3_finalize(pragma) != SQLITE_OK) { enabled = 0; }
    if (enabled == 0)
    {
        (void)fprintf(stderr, "gargantua: sqlite: foreign keys unavailable\n");
        (void)sqlite3_close(g_db);
        g_db = NULL;
        db_unlock();
        return -1;
    }
    sqlite3_progress_handler(g_db, 1000, sqlite_progress_handler, NULL);
    (void)sqlite3_set_authorizer(g_db, sqlite_authorizer, NULL);
    db_unlock();
    return 0;
}

void db_close(void)
{
    db_abort_all();
    assert(DB_MAX_SQL > 0);

    if (db_lock() != 0) { return; }
    if (g_db != NULL)
    {
        (void)sqlite3_close(g_db);
        g_db = NULL;
    }
    db_unlock();
}

static int bind_arguments(sqlite3_stmt *st, const SqlArg *args, int nargs)
{
    assert(st != NULL);
    assert(nargs >= 0);

    for (int i = 0; (i < nargs) && (i < DB_MAX_ARGS); i++)
    {
        int rc;
        if (args[i].kind == FIELD_STR)
        {
            rc = (args[i].s != NULL)
                     ? sqlite3_bind_text(st, i + 1, args[i].s, -1, SQLITE_TRANSIENT)
                     : sqlite3_bind_null(st, i + 1);
        }
        else if ((args[i].kind == FIELD_INT) || (args[i].kind == FIELD_BOOL))
        {
            rc = sqlite3_bind_int(st, i + 1, args[i].i);
        }
        else if (args[i].kind == FIELD_LONG)
        {
            rc = sqlite3_bind_int64(st, i + 1, (sqlite3_int64)args[i].l);
        }
        else if ((args[i].kind == FIELD_DOUBLE) && (isfinite(args[i].d) != 0))
        {
            rc = sqlite3_bind_double(st, i + 1, args[i].d);
        }
        else { return -1; }
        if (rc != SQLITE_OK) { return -1; }
    }
    return 0;
}

static sqlite3_stmt *prepare_statement(str sql, const SqlArg *args, int nargs)
{
    assert(sql != NULL);
    assert(DB_MAX_SQL > 0);

    if (g_db == NULL)
    {
        (void)fprintf(stderr, "gargantua: the database is not open\n");
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL;
    }

    size_t len = 0u;
    while (len < DB_MAX_SQL && sql[len]) { len++; }
    if (len >= DB_MAX_SQL) { if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL; }
    if (t_depth > 0 && sqlite3_get_autocommit(g_db) != 0) { if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL; }
    statement_deadline();
    sqlite3_stmt *st = NULL;
    const char *tail = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, &tail) != SQLITE_OK)
    {
        db_failure();
        (void)fprintf(stderr, "gargantua: sqlite prepare_statement failed (%d)\n", sqlite3_extended_errcode(g_db));
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL;
    }
    while (tail != NULL && (*tail == ' ' || *tail == '\t' || *tail == '\r' || *tail == '\n')) { tail++; }
    if (st == NULL || (tail != NULL && *tail != '\0'))
    {
        (void)sqlite3_finalize(st);
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL;
    }
    if (sqlite3_bind_parameter_count(st) != nargs)
    {
        (void)fprintf(stderr, "gargantua: sqlite: placeholders and args differ\n");
        (void)sqlite3_finalize(st);
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL;
    }
    if (bind_arguments(st, args, nargs) != 0)
    {
        (void)sqlite3_finalize(st);
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; } return NULL;
    }
    return st;
}

int db_execute(str sql, const SqlArg *args, int nargs)
{
    if (sql == NULL || nargs < 0) { return -1; }

    if (nargs > DB_MAX_ARGS || (nargs > 0 && args == NULL)) { return -1; }
    if (db_lock() != 0) { return -1; }
    g_last_id = -1;
    sqlite3_stmt *st = prepare_statement(sql, args, nargs);
    if (st == NULL)
    {
        db_unlock();
        return -1;
    }

    int rc = sqlite3_step(st);
    if ((rc != SQLITE_DONE) && (rc != SQLITE_ROW))
    {
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; }
        db_failure();
        (void)fprintf(stderr, "gargantua: sqlite operation failed (%d)\n", sqlite3_extended_errcode(g_db));
        (void)sqlite3_finalize(st);
        db_unlock();
        return -1;
    }

    int changed = sqlite3_changes(g_db);
    const char *actual = sqlite3_sql(st);
    while ((actual != NULL) && ((*actual == ' ') || (*actual == '\t') || (*actual == '\r') || (*actual == '\n'))) { actual++; }
    sqlite3_int64 rowid = sqlite3_last_insert_rowid(g_db);
    if ((changed > 0) && (actual != NULL) && (strncasecmp(actual, "INSERT", 6u) == 0) && ((actual[6] == ' ') || (actual[6] == '\t') || (actual[6] == '\r') || (actual[6] == '\n')) && (rowid >= (sqlite3_int64)INT_MIN) && (rowid <= (sqlite3_int64)INT_MAX)) { g_last_id = (int)rowid; }
    (void)sqlite3_finalize(st);
    db_unlock();
    return changed;
}

int db_last_id(void)
{
    assert(DB_MAX_ARGS > 0);

    return g_last_id;
}

int db_insert_id(str sql, const SqlArg *args, int nargs)
{
    g_last_id = -1;
    if ((sql == NULL) || (nargs < 0) || (nargs > DB_MAX_ARGS) || ((nargs > 0) && (args == NULL)) || (sqlite3_libversion_number() < 3035000)) { return -1; }

    size_t len = 0u;
    while ((len < DB_MAX_SQL) && (sql[len] != '\0')) { len++; }
    if (len >= DB_MAX_SQL) { return -1; }
    const char *start = sql;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') { start++; }
    if ((strncasecmp(start, "INSERT", 6u) != 0) || (start[6] != ' ' && start[6] != '\t' && start[6] != '\r' && start[6] != '\n')) { return -1; }

    if (db_lock() != 0) { return -1; }
    sqlite3_stmt *st = prepare_statement(sql, args, nargs);
    if (st == NULL)
    {
        db_unlock();
        return -1;
    }

    int id = -1;
    if ((sqlite3_stmt_readonly(st) == 0) && (sqlite3_column_count(st) == 1) && (sqlite3_step(st) == SQLITE_ROW) && (sqlite3_column_type(st, 0) == SQLITE_INTEGER))
    {
        sqlite3_int64 value = sqlite3_column_int64(st, 0);
        if ((value > 0) && (value <= (sqlite3_int64)INT_MAX) && (sqlite3_step(st) == SQLITE_DONE) && (sqlite3_changes(g_db) == 1)) { id = (int)value; }
    }
    if (id < 0) { db_failure(); }
    if (sqlite3_finalize(st) != SQLITE_OK) { id = -1; db_failure(); }
    if (id < 0 && t_depth && !t_failed_level) { t_failed_level = t_depth; }
    g_last_id = id;
    db_unlock();
    return id;
}

static size_t column_size_for_kind(FieldKind kind)
{
    if (kind == FIELD_INT) { return sizeof(int); }
    if (kind == FIELD_LONG) { return sizeof(long); }
    if (kind == FIELD_DOUBLE) { return sizeof(double); }
    if (kind == FIELD_BOOL) { return sizeof(bool); }
    if (kind == FIELD_STR) { return sizeof(str); }
    return 0u;
}

static int is_safe_identifier(const char *name)
{
    if ((name == NULL) || ((name[0] < 'A' || name[0] > 'Z') && (name[0] < 'a' || name[0] > 'z') && name[0] != '_')) { return 0; }
    for (size_t i = 1u; i < (1024u * 1024u); i++)
    {
        char c = name[i];
        if (c == '\0') { return 1; }
        if ((c < 'A' || c > 'Z') && (c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_') { return 0; }
    }
    return 0;
}

static int type_valid(const TypeInfo *type)
{
    if ((type == NULL) || !is_safe_identifier(type->name) || (type->size == 0u) || (type->field_count == 0u) || (type->field_count > 64u) || (type->fields == NULL)) { return 0; }
    unsigned keys = 0u;
    for (unsigned i = 0u; i < type->field_count; i++)
    {
        const FieldInfo *f = &type->fields[i];
        size_t need = column_size_for_kind(f->kind);
        if (!is_safe_identifier(f->name) || (need == 0u) || (f->size != need) || (f->offset > type->size) || (need > (size_t)(type->size - f->offset)) || (f->nested != NULL) || ((f->flags & ~(unsigned)(FIELD_PRIMARY_KEY | FIELD_NOT_NULL | FIELD_UNIQUE)) != 0u)) { return 0; }
        if ((f->flags & FIELD_PRIMARY_KEY) != 0u)
        {
            if ((f->kind != FIELD_INT) || (++keys > 1u)) { return 0; }
        }
        if ((f->references != NULL) || (f->reference_key != NULL))
        {
            if ((f->kind != FIELD_INT) || !is_safe_identifier(f->references) || !is_safe_identifier(f->reference_key)) { return 0; }
        }
        for (unsigned j = 0u; j < i; j++)
        {
            const FieldInfo *other = &type->fields[j];
            if ((strcmp(f->name, other->name) == 0) || ((f->offset < (size_t)other->offset + other->size) && (other->offset < (size_t)f->offset + f->size))) { return 0; }
        }
    }
    return 1;
}

static int read_text_column(sqlite3_stmt *st, int c, int sql_type, void *slot)
{
    assert(st != NULL);
    assert(slot != NULL);

    if (sql_type != SQLITE_TEXT) { return -1; }

    const unsigned char *text = sqlite3_column_text(st, c);
    if (text == NULL) { return -1; }

    str value = arena_intern((const char *)text);
    if (value == NULL) { return -1; }

    memcpy(slot, &value, sizeof(value));

    return 0;
}

static int read_integer_column(sqlite3_stmt *st, int c, int sql_type, FieldKind kind, void *slot)
{
    assert(st != NULL);
    assert(slot != NULL);

    if (sql_type != SQLITE_INTEGER) { return -1; }

    sqlite3_int64 raw = sqlite3_column_int64(st, c);

    if (kind == FIELD_INT)
    {
        if ((raw < (sqlite3_int64)INT_MIN) || (raw > (sqlite3_int64)INT_MAX)) { return -1; }
        int value = (int)raw;
        memcpy(slot, &value, sizeof(value));
        return 0;
    }

    if (kind == FIELD_BOOL)
    {
        if ((raw != 0) && (raw != 1)) { return -1; }
        bool value = (raw != 0);
        memcpy(slot, &value, sizeof(value));
        return 0;
    }

    if ((sizeof(long) < sizeof(sqlite3_int64)) && ((raw < (sqlite3_int64)LONG_MIN) || (raw > (sqlite3_int64)LONG_MAX))) { return -1; }

    long value = (long)raw;
    memcpy(slot, &value, sizeof(value));

    return 0;
}

static int read_double_column(sqlite3_stmt *st, int c, int sql_type, void *slot)
{
    assert(st != NULL);
    assert(slot != NULL);

    if ((sql_type != SQLITE_FLOAT) && (sql_type != SQLITE_INTEGER)) { return -1; }

    double value = sqlite3_column_double(st, c);
    if (isfinite(value) == 0) { return -1; }

    memcpy(slot, &value, sizeof(value));

    return 0;
}

static int read_column(sqlite3_stmt *st, int c, const FieldInfo *field, void *slot)
{
    assert(st != NULL);
    assert(field != NULL);
    assert(slot != NULL);

    int sql_type = sqlite3_column_type(st, c);

    switch (field->kind)
    {
        case FIELD_STR:
            return read_text_column(st, c, sql_type, slot);

        case FIELD_INT:
        case FIELD_BOOL:
        case FIELD_LONG:
            return read_integer_column(st, c, sql_type, field->kind, slot);

        case FIELD_DOUBLE:
            return read_double_column(st, c, sql_type, slot);

        default:
            break;
    }

    return 0;
}

static int field_slot_valid(const TypeInfo *type, const FieldInfo *field)
{
    assert(type != NULL);
    assert(field != NULL);

    size_t need = column_size_for_kind(field->kind);

    return !((need == 0u) || ((size_t)field->size != need) || ((size_t)field->offset > (size_t)type->size) || (need > ((size_t)type->size - (size_t)field->offset)));
}

static int read_field(sqlite3_stmt *st, int cols, const FieldInfo *field, void *obj)
{
    assert(st != NULL);
    assert(field != NULL);

    int found = 0;

    for (int c = 0; (c < cols) && (c < 64); c++)
    {
        const char *name = sqlite3_column_name(st, c);
        if ((name == NULL) || (strcmp(name, field->name) != 0)) { continue; }
        if (found != 0) { return -1; }
        found = 1;

        if (sqlite3_column_type(st, c) == SQLITE_NULL)
        {
            if ((field->flags & (unsigned)FIELD_NOT_NULL) != 0u) { return -1; }
            continue;
        }

        char *base = obj;
        if (read_column(st, c, field, base + field->offset) != 0) { return -1; }
    }

    return (found == 0) ? -1 : 0;
}

static int row_to_struct(sqlite3_stmt *st, const TypeInfo *type, void *obj)
{
    assert(st != NULL);
    assert(type != NULL);

    if ((type->size == 0u) || (type->field_count == 0u) || (type->field_count > 64u) || (type->fields == NULL)) { return -1; }

    memset(obj, 0, type->size);
    int cols = sqlite3_column_count(st);

    for (unsigned f = 0u; (f < type->field_count) && (f < 64u); f++)
    {
        const FieldInfo *field = &type->fields[f];

        if (field_slot_valid(type, field) == 0) { return -1; }
        if (read_field(st, cols, field, obj) != 0) { return -1; }
    }

    return 0;
}

int db_query_one(const TypeInfo *type, void *out, str sql, const SqlArg *args, int nargs)
{
    if (!type_valid(type) || (out == NULL) || (sql == NULL) || (nargs < 0) || (nargs > DB_MAX_ARGS) || ((nargs > 0) && (args == NULL))) { return -1; }
    memset(out, 0, type->size);

    if (db_lock() != 0) { return -1; }
    sqlite3_stmt *st = prepare_statement(sql, args, nargs);
    if (st == NULL)
    {
        db_unlock();
        return -1;
    }

    int rc    = sqlite3_step(st);
    int found = 0;

    if (rc == SQLITE_ROW)
    {
        found = (row_to_struct(st, type, out) == 0) ? 1 : -1;
    }
    else if (rc != SQLITE_DONE)
    {
        db_failure();
        if (t_depth && !t_failed_level) { t_failed_level = t_depth; }
        (void)sqlite3_finalize(st);
        db_unlock();
        return -1;
    }

    (void)sqlite3_finalize(st);
    db_unlock();
    if (found < 0 && t_depth && !t_failed_level) { t_failed_level = t_depth; }
    return found;
}

int db_query_many(const TypeInfo *type, void *rows, int max, str sql, const SqlArg *args, int nargs)
{
    if (!type_valid(type) || (rows == NULL) || (sql == NULL) || (nargs < 0) || (nargs > DB_MAX_ARGS) || ((nargs > 0) && (args == NULL)) || (max <= 0) || (max > SQLITE_MAX_ROWS)) { return -1; }

    if (db_lock() != 0) { return -1; }
    sqlite3_stmt *st = prepare_statement(sql, args, nargs);
    if (st == NULL)
    {
        db_unlock();
        return -1;
    }

    char *base  = rows;
    int   count = 0;

    int rc = SQLITE_ROW;
    while ((count < max) && ((rc = sqlite3_step(st)) == SQLITE_ROW))
    {
        if (row_to_struct(st, type, base + ((size_t)count * type->size)) != 0)
        {
            if (t_depth && !t_failed_level) { t_failed_level = t_depth; }
            (void)sqlite3_finalize(st);
            db_unlock();
            return -1;
        }
        count++;
    }

    if (rc != SQLITE_DONE && count != max) { db_failure(); }
    (void)sqlite3_finalize(st);
    db_unlock();
    if (rc != SQLITE_DONE && count != max && t_depth && !t_failed_level) { t_failed_level = t_depth; }
    return (rc == SQLITE_DONE || count == max) ? count : -1;
}

static int run_control_statement(const char *sql)
{
    t_control = 1;
    statement_deadline();
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, NULL);
    t_control = 0;
    if (rc != SQLITE_OK) { db_failure(); }
    return rc == SQLITE_OK ? 0 : -1;
}

int db_begin(void)
{
    if (t_depth >= 16 || db_lock() != 0) { return -1; }
    if (g_db == NULL || (t_depth > 0 && sqlite3_get_autocommit(g_db)))
    {
        db_unlock();
        return -1;
    }
    char sql[64];
    if (t_depth == 0) { (void)snprintf(sql, sizeof(sql), "BEGIN IMMEDIATE"); }
    else { (void)snprintf(sql, sizeof(sql), "SAVEPOINT wrap_sp_%d", t_depth + 1); }
    if (run_control_statement(sql) != 0) { db_unlock(); return -1; }
    t_depth++;
    return 0;
}

int db_commit(void)
{
    if (t_depth == 0 || t_failed_level || g_db == NULL || sqlite3_get_autocommit(g_db)) { return -1; }
    char sql[64];
    if (t_depth == 1) { (void)snprintf(sql, sizeof(sql), "COMMIT"); }
    else { (void)snprintf(sql, sizeof(sql), "RELEASE wrap_sp_%d", t_depth); }
    if (run_control_statement(sql) != 0) { return -1; }
    t_depth--;
    db_unlock();
    return 0;
}

int db_rollback(void)
{
    if (t_depth == 0 || g_db == NULL) { return -1; }
    int rc = 0;
    if (!sqlite3_get_autocommit(g_db))
    {
        char sql[128];
        if (t_depth == 1) { (void)snprintf(sql, sizeof(sql), "ROLLBACK"); }
        else
        {
            (void)snprintf(sql, sizeof(sql), "ROLLBACK TO wrap_sp_%d; RELEASE wrap_sp_%d", t_depth, t_depth);
        }
        rc = run_control_statement(sql);
    }
    if (rc == 0)
    {
        if (t_failed_level >= t_depth) { t_failed_level = 0; }
        if (sqlite3_get_autocommit(g_db)) { t_depth = 0; }
        else { t_depth--; }
        db_unlock();
    }
    return rc;
}

void db_abort_all(void)
{
    if (t_depth == 0) { return; }
    sqlite3_progress_handler(g_db, 0, NULL, NULL);
    int rc = run_control_statement("ROLLBACK");
    if (rc != 0 && !sqlite3_get_autocommit(g_db))
    {
        (void)sqlite3_close_v2(g_db);
        g_db = NULL;
    }
    if (g_db != NULL) { sqlite3_progress_handler(g_db, 1000, sqlite_progress_handler, NULL); }
    t_depth = 0;
    t_failed_level = 0;
    db_unlock();
}

int db_depth(void)
{
    return t_depth;
}

int db_ready(void)
{
    if (db_lock() != 0) { return 0; }
    int ready = g_db != NULL;
    db_unlock();
    return ready;
}

int db_script(const char *sql)
{
    if (sql == NULL || t_depth == 0 || g_db == NULL) { return -1; }
    statement_deadline();
    const char *cursor = sql;
    for (int i = 0; i < 256; i++)
    {
        if (*cursor == '\0') { return 0; }
        sqlite3_stmt *st = NULL;
        const char *tail = NULL;
        if (sqlite3_prepare_v2(g_db, cursor, -1, &st, &tail) != SQLITE_OK)
        {
            (void)sqlite3_finalize(st);
            return -1;
        }
        if (tail == NULL || tail <= cursor)
        {
            (void)sqlite3_finalize(st);
            return -1;
        }
        cursor = tail;
        if (st == NULL) { continue; }
        int rc = sqlite3_step(st);
        int done = rc == SQLITE_DONE;
        if (sqlite3_finalize(st) != SQLITE_OK || !done) { return -1; }
    }
    return *cursor == '\0' ? 0 : -1;
}

int db_migration_count(void)
{
    if (t_depth == 0) { return -1; }
    if (db_execute("CREATE TABLE IF NOT EXISTS _gargantua_migrations " "(version INTEGER PRIMARY KEY, name TEXT NOT NULL, sql TEXT NOT NULL)", SQL_NOARGS) < 0) { return -1; }
    sqlite3_stmt *st = prepare_statement("SELECT count(*) FROM _gargantua_migrations", NULL, 0);
    if (st == NULL) { return -1; }
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    if (sqlite3_finalize(st) != SQLITE_OK) { return -1; }
    return n;
}

int db_migration(int version, const char *name, const char *sql, int apply)
{
    if (t_depth == 0 || version <= 0 || name == NULL || sql == NULL) { return -1; }
    sqlite3_stmt *st = prepare_statement("SELECT name, sql FROM _gargantua_migrations WHERE version = ?", SQL_ARGS(SQL_INT(version)));
    if (st == NULL) { return -1; }
    int step = sqlite3_step(st);
    int exists = step == SQLITE_ROW;
    int valid = step == SQLITE_DONE;
    if (exists)
    {
        const char *saved_name = (const char *)sqlite3_column_text(st, 0);
        const char *saved_sql = (const char *)sqlite3_column_text(st, 1);
        valid = saved_name != NULL && saved_sql != NULL &&
            (size_t)sqlite3_column_bytes(st, 0) == strlen(name) &&
            (size_t)sqlite3_column_bytes(st, 1) == strlen(sql) &&
            strcmp(saved_name, name) == 0 && strcmp(saved_sql, sql) == 0;
    }
    if (sqlite3_finalize(st) != SQLITE_OK || !valid) { return -1; }
    if (exists) { return 1; }
    if (!apply) { return 0; }
    if (db_script(sql) != 0) { return -1; }
    return db_execute("INSERT INTO _gargantua_migrations(version,name,sql) VALUES (?,?,?)", SQL_ARGS(SQL_INT(version), SQL_TEXT(name), SQL_TEXT(sql))) == 1 ? 1 : -1;
}
