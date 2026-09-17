#include <rest_controller.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

$get("/benchmark/rss")
long benchmark_rss(void)
{
#if defined(__APPLE__)
    struct mach_task_basic_info info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
    {
        return -1;
    }
    return (long)(info.resident_size / 1024u);
#elif defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "r");
    if (file == NULL)
    {
        return -1;
    }
    unsigned long total = 0;
    unsigned long resident = 0;
    int fields = fscanf(file, "%lu %lu", &total, &resident);
    (void)fclose(file);
    long page = sysconf(_SC_PAGESIZE);
    if (fields != 2 || page <= 0 || resident > (unsigned long)LONG_MAX / (unsigned long)page)
    {
        return -1;
    }
    return (long)resident * page / 1024L;
#else
    return -1;
#endif
}
