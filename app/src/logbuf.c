#include "logbuf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/log.h"

enum { CAPACITY = 2 * 1024 * 1024 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char buffer[CAPACITY];
static size_t used;
static unsigned lost_bytes;
static bool enabled = true, buffering;

/* Every stdout/stderr write goes through xsdk_log_write_bytes (xsdk src/file.c); -Wl,--wrap sends it here. */
void __real_xsdk_log_write_bytes(const char *bytes, size_t length_bytes);

void __wrap_xsdk_log_write_bytes(const char *bytes, size_t length_bytes)
{
    pthread_mutex_lock(&lock);
    if (!buffering) {
        pthread_mutex_unlock(&lock);
        __real_xsdk_log_write_bytes(bytes, length_bytes);
        return;
    }
    if (used + length_bytes <= CAPACITY) {
        memcpy(buffer + used, bytes, length_bytes);
        used += length_bytes;
    } else {
        lost_bytes += (unsigned)length_bytes;
    }
    pthread_mutex_unlock(&lock);
}

void logbuf_set_enabled(bool on)
{
    enabled = on;
}

void logbuf_begin(void)
{
    pthread_mutex_lock(&lock);
    buffering = enabled;
    pthread_mutex_unlock(&lock);
}

void logbuf_end(void)
{
    pthread_mutex_lock(&lock);
    bool was = buffering;
    buffering = false;
    size_t n = used;
    unsigned lost = lost_bytes;
    used = 0;
    lost_bytes = 0;
    pthread_mutex_unlock(&lock);
    if (!was)
        return;
    /* In pieces: xsdk writes a "line" of up to its own buffer size at a time. */
    for (size_t at = 0; at < n;) {
        size_t chunk = n - at < 4096 ? n - at : 4096;
        __real_xsdk_log_write_bytes(buffer + at, chunk);
        at += chunk;
    }
    if (lost)
        printf("[W] logbuf: %u bytes of log dropped (buffer full)\n", lost);
}
