#include "placement.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "xsdk/thread.h"

/* Network and housekeeping threads share core 0 with the main (render) thread. They run at a raised priority:
 * they mostly wait in poll/recv, and when packets arrive they must drain the sockets before anything else on their
 * hardware thread, or the console's network buffers fill. */
enum { DEFAULT_HW_THREAD = 1, NETWORK_PRIORITY = 2 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t workers[6];
static int worker_count;
static int next_worker;
static int worker_priority;
static int default_priority = NETWORK_PRIORITY;

void placement_set_default_priority(int priority)
{
    default_priority = priority;
}

void placement_set_worker_priority(int priority)
{
    worker_priority = priority;
}

void placement_set_workers(const uint32_t *hardware_threads, int count)
{
    pthread_mutex_lock(&lock);
    worker_count = count > 6 ? 6 : count;
    for (int i = 0; i < worker_count; i++)
        workers[i] = hardware_threads[i];
    next_worker = 0;
    pthread_mutex_unlock(&lock);
}

void placement_move_self(uint32_t hardware_thread)
{
    NTSTATUS st = xsdk_thread_move(hardware_thread);
    if (!NT_SUCCESS(st))
        printf("[W] placement: move to hw %u failed: 0x%08X\n", (unsigned)hardware_thread, (unsigned)st);
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    int ret = __real_pthread_create(thread, attr, start, arg);
    if (ret != 0)
        return ret;
    pthread_mutex_lock(&lock);
    bool is_worker = worker_count != 0;
    uint32_t hw = is_worker ? workers[next_worker++ % worker_count] : DEFAULT_HW_THREAD;
    pthread_mutex_unlock(&lock);
    HANDLE handle = xsdk_pthread_handle(*thread);
    NTSTATUS st = xsdk_thread_place(handle, hw);
    if (!NT_SUCCESS(st))
        printf("[W] placement: hw %u failed: 0x%08X\n", (unsigned)hw, (unsigned)st);
    if (is_worker && worker_priority != 0) {
        st = xsdk_thread_set_priority(handle, worker_priority, NULL);
        if (!NT_SUCCESS(st))
            printf("[W] placement: worker priority failed: 0x%08X\n", (unsigned)st);
    }
    if (!is_worker) {
        st = xsdk_thread_set_priority(handle, default_priority, NULL);
        if (!NT_SUCCESS(st))
            printf("[W] placement: priority failed: 0x%08X\n", (unsigned)st);
    }
    return 0;
}
