/**
 * @file  utils_helper.h
 * @brief This is the header file to resolve pthread datatype and affinity issues in src/includes/utils.h
 */

#ifndef UTILS_HELPER_H
#define UTILS_HELPER_H

#ifdef _WIN32

struct sched_param {
  int sched_priority;
};

#include "pthread.h"
#include <winsock2.h>
#include <errno.h>
#include "arch.h"
typedef rte_cpuset_t cpu_set_t;
const int SCHED_FIFO = 1;

static __inline int lc_set_errno(int result)
{
    if (result != 0) {
        errno = result;
        return -1;
    }
    return 0;
}

static __inline int sched_priority_to_os_priority(int priority)
{
    /* THREAD_PRIORITY_TIME_CRITICAL (15) */
    /* THREAD_PRIORITY_HIGHEST (12, 13, 14) */
    /* THREAD_PRIORITY_ABOVE_NORMAL (9, 10, 11) */
    /* THREAD_PRIORITY_NORMAL (8) */
    /* THREAD_PRIORITY_BELOW_NORMAL (5, 6, 7) */
    /* THREAD_PRIORITY_LOWEST (2, 3, 4) */
    /* THREAD_PRIORITY_IDLE (1) */

    if (priority >= 15)
        return THREAD_PRIORITY_TIME_CRITICAL;
    else if (priority >= 12)
        return THREAD_PRIORITY_HIGHEST;
    else if (priority >= 9)
        return THREAD_PRIORITY_ABOVE_NORMAL;
    else if (priority >= 8)
        return THREAD_PRIORITY_NORMAL;
    else if (priority >= 5)
        return THREAD_PRIORITY_BELOW_NORMAL;
    else if (priority >= 2)
        return THREAD_PRIORITY_LOWEST;
    else
        return THREAD_PRIORITY_IDLE;
}

// int pthread_setschedprio(pthread_t thread, int priority)
// {
//     HANDLE handle;
//     arch_thread_info *pv = (arch_thread_info *) thread;

//     if (pv != NULL) handle = pv->handle;
//     else handle = GetCurrentThread();

//     if (SetThreadPriority(handle, sched_priority_to_os_priority(priority)) == 0)
//         return lc_set_errno(ESRCH);

//     return 0;
// }

// int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
// {
//     if (param != NULL)
//         return pthread_setschedprio(thread, param->sched_priority);

//     return 0;
// }



#endif

#endif // UTILS_HELPER_H