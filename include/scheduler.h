#ifndef SCHEDULER_H
#define SCHEDULER_H

#define SCHEDULER_MAX_TASKS 16
#define SCHEDULER_ARENA     (256 * 1024)

typedef void (*TaskBody)(void);

typedef struct
{
    const char *name;
    long        interval_ms;
    TaskBody    body;
} Task;

const Task *task_table(void);
int         task_count(void);

int  scheduler_start(void);
void scheduler_stop(void);

#endif
