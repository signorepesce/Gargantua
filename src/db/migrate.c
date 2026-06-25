#include "migrate.h"
#include "db.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIGRATE_MAX 128
#define MIGRATE_FILE_MAX 65536u
#define MIGRATE_TOTAL_MAX (1024u * 1024u)

typedef struct
{
    int version;
    char name[128];
    char *sql;
} Migration;

static int migration_version(const char *name)
{
    if (*name < '1' || *name > '9') { return -1; }
    unsigned version = 0u;
    size_t i = 0u;
    for (; i < 10u && name[i] >= '0' && name[i] <= '9'; i++)
    {
        unsigned digit = (unsigned)(name[i] - '0');
        if (version > ((unsigned)INT_MAX - digit) / 10u) { return -1; }
        version = version * 10u + digit;
    }
    if (name[i] != '_' || name[i + 1u] != '_') { return -1; }
    size_t len = strlen(name);
    if (len <= i + 6u || len >= 128u || strcmp(name + len - 4u, ".sql") != 0) { return -1; }
    for (size_t j = i + 2u; j < len - 4u; j++)
    {
        char c = name[j];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') && (c < '0' || c > '9') && c != '_' && c != '-') { return -1; }
    }
    return (int)version;
}

static char *read_sql(int directory, const char *name, size_t *total)
{
    if (directory < 0) { return NULL; }
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { return NULL; }
    struct stat st;
    char *sql = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (off_t)MIGRATE_FILE_MAX && (size_t)st.st_size <= MIGRATE_TOTAL_MAX - *total)
    {
        size_t len = (size_t)st.st_size;
        sql = malloc(len + 1u);
        if (sql != NULL)
        {
            size_t at = 0u;
            for (size_t steps = 0u; at < len && steps < len; steps++)
            {
                ssize_t n = read(fd, sql + at, len - at);
                if (n <= 0) { break; }
                at += (size_t)n;
            }
            char extra;
            if (at != len || read(fd, &extra, 1u) != 0 || memchr(sql, '\0', len) != NULL)
            {
                free(sql);
                sql = NULL;
            }
            else { sql[len] = '\0'; *total += len; }
        }
    }
    (void)close(fd);
    return sql;
}

static int apply_migrations(Migration *files, int count)
{
    if (db_begin() != 0) { return -1; }
    int applied = db_migration_count();
    if (applied < 0 || applied > count) { db_abort_all(); return -1; }
    for (int i = 0; i < count; i++)
    {
        int result = db_migration(files[i].version, files[i].name, files[i].sql, 0);
        if (result != (i < applied ? 1 : 0)) { db_abort_all(); return -1; }
    }
    for (int i = applied; i < count; i++)
    {
        if (db_migration(files[i].version, files[i].name, files[i].sql, 1) != 1)
        {
            db_abort_all();
            return -1;
        }
    }
    if (db_commit() != 0) { db_abort_all(); return -1; }
    return 0;
}

int migrate_run(const char *directory)
{
    if (directory == NULL || *directory == '\0') { return 0; }
    DIR *dir = opendir(directory);
    if (dir == NULL)
    {
        if (errno != ENOENT) { return -1; }
        return apply_migrations(NULL, 0);
    }
    Migration *files = calloc(MIGRATE_MAX, sizeof(*files));
    if (files == NULL) { (void)closedir(dir); return -1; }
    int count = 0;
    int result = 0;
    size_t total = 0u;
    int exhausted = 1;
    for (int scan = 0; scan < 1024; scan++)
    {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) { exhausted = 0; if (errno != 0) { result = -1; } break; }
        if (entry->d_name[0] == '.') { continue; }
        size_t len = strlen(entry->d_name);
        if (len < 4u || strcmp(entry->d_name + len - 4u, ".sql") != 0) { continue; }
        int version = migration_version(entry->d_name);
        if (version < 0 || count >= MIGRATE_MAX) { result = -1; break; }
        int pos = count;
        while (pos > 0 && files[pos - 1].version > version) { pos--; }
        if ((pos > 0 && files[pos - 1].version == version) || (pos < count && files[pos].version == version)) { result = -1; break; }
        char *sql = read_sql(dirfd(dir), entry->d_name, &total);
        if (sql == NULL) { result = -1; break; }
        memmove(&files[pos + 1], &files[pos], (size_t)(count - pos) * sizeof(*files));
        files[pos].version = version;
        memcpy(files[pos].name, entry->d_name, len + 1u);
        files[pos].sql = sql;
        count++;
    }
    if (exhausted != 0) { result = -1; }
    if (closedir(dir) != 0) { result = -1; }
    if (result == 0) { result = apply_migrations(files, count); }
    for (int i = 0; i < count; i++) { free(files[i].sql); }
    free(files);
    if (result != 0) { (void)fprintf(stderr, "gargantua: migration validation or execution failed\n"); }
    return result;
}
