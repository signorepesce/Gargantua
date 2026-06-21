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

