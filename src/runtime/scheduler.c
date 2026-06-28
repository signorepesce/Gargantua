#include "scheduler.h"
#include "arena.h"
#include "db.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct
{
    const Task *task;
    Arena arena;
    unsigned char backing[SCHEDULER_ARENA];
} TaskRunner;

static TaskRunner task_runners[SCHEDULER_MAX_TASKS];
static pthread_t task_threads[SCHEDULER_MAX_TASKS];
static int started_threads;
static int scheduler_running;
static _Atomic int scheduler_stopping;
static pthread_mutex_t sleep_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sleep_wake = PTHREAD_COND_INITIALIZER;

static int scheduler_wait(long milliseconds)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) { return -1; }
    deadline.tv_sec += (time_t)(milliseconds / 1000L);
    deadline.tv_nsec += (milliseconds % 1000L) * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    if (pthread_mutex_lock(&sleep_lock) != 0) { return -1; }
    int result = 0;
    while (scheduler_stopping == 0)
    {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        {
            result = -1;
            break;
        }
        long left = (long)(deadline.tv_sec - now.tv_sec) * 1000L +
                    (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (left <= 0L) { break; }
        struct timespec wait = {0, ((left < 100L) ? left : 100L) * 1000000L};
#ifdef __APPLE__
        int status = pthread_cond_timedwait_relative_np(&sleep_wake, &sleep_lock, &wait);
#else
        struct timespec until;
        if (clock_gettime(CLOCK_REALTIME, &until) != 0)
        {
            result = -1;
            break;
        }
        until.tv_nsec += wait.tv_nsec;
        until.tv_sec += until.tv_nsec / 1000000000L;
        until.tv_nsec %= 1000000000L;
        int status = pthread_cond_timedwait(&sleep_wake, &sleep_lock, &until);
#endif
        if ((status != 0) && (status != ETIMEDOUT))
        {
            result = -1;
            break;
        }
    }
    if (scheduler_stopping != 0) { result = -1; }
    (void)pthread_mutex_unlock(&sleep_lock);
    return result;
}

static int scheduler_run_task(TaskRunner *runner)
{
    assert(runner != NULL);
    arena_bind(&runner->arena);
    request_bind(NULL);
    auth_reset();
    response_reset();
    request_fail_reset();
    int result = dispatch_task(runner->task->body);
    if (result != 0) { log_write("warn", "task '%s' failed: %s", runner->task->name, request_fail_message()); }
    arena_bind(NULL);
    request_fail_reset();
    response_reset();
    auth_reset();
    if (arena_reset(&runner->arena) != 0)
    {
        log_write("error", "task '%s' damaged its arena", runner->task->name);
        scheduler_stopping = 1;
        return -1;
    }
    return result;
}

static void *scheduler_run_repeatedly(void *argument)
{
    TaskRunner *runner = argument;
    assert(runner != NULL);
    while (scheduler_stopping == 0)
    {
        (void)scheduler_run_task(runner);
        if (scheduler_wait(runner->task->interval_ms) != 0) { break; }
    }
    return NULL;
}

static int scheduler_prepare(const Task *tasks, int count)
{
    if ((count < 0) || (count > SCHEDULER_MAX_TASKS) || ((count > 0) && (tasks == NULL))) { return -1; }
    for (int i = 0; i < count; i++)
    {
        if ((tasks[i].body == NULL) || (tasks[i].name == NULL) || (tasks[i].interval_ms < 0L) || (tasks[i].interval_ms > 86400000L) || ((tasks[i].interval_ms > 0L) && (tasks[i].interval_ms < 100L))) { return -1; }
        task_runners[i].task = &tasks[i];
        if (arena_init(&task_runners[i].arena, task_runners[i].backing, sizeof(task_runners[i].backing)) != 0) { return -1; }
    }
    return 0;
}

int scheduler_start(void)
{
    if (scheduler_running != 0) { return -1; }
    const Task *tasks = task_table();
    int count = task_count();
    if (scheduler_prepare(tasks, count) != 0) { return -1; }
    scheduler_running = 1;
    scheduler_stopping = 0;
    for (int i = 0; i < count; i++)
    {
        if ((tasks[i].interval_ms == 0L) && (scheduler_run_task(&task_runners[i]) != 0))
        {
            scheduler_stop();
            return -1;
        }
    }
    for (int i = 0; i < count; i++)
    {
        if (tasks[i].interval_ms == 0L) { continue; }
        if (pthread_create(&task_threads[started_threads], NULL, scheduler_run_repeatedly, &task_runners[i]) != 0)
        {
            scheduler_stop();
            return -1;
        }
        started_threads++;
    }
    return 0;
}

void scheduler_stop(void)
{
    if (pthread_mutex_lock(&sleep_lock) != 0) { return; }
    scheduler_stopping = 1;
    (void)pthread_cond_broadcast(&sleep_wake);
    (void)pthread_mutex_unlock(&sleep_lock);
    for (int i = 0; i < started_threads; i++) { (void)pthread_join(task_threads[i], NULL); }
    started_threads = 0;
    scheduler_running = 0;
}
