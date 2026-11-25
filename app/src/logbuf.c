#include "logbuf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "xsdk/log.h"

enum { CAPACITY = 2 * 1024 * 1024 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char buffer[CAPACITY];
static size_t used;
static unsigned lost_bytes;
static bool enabled = true, buffering, logging = true;
/* The newest log text, for the About tab's log view: a ring of RECENT bytes. */
enum { RECENT = 8192 };
static char recent[RECENT];
static size_t recent_end, recent_used;

static void keep_recent(const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        recent[recent_end] = bytes[i];
        recent_end = (recent_end + 1) % RECENT;
    }
    recent_used = recent_used + n > RECENT ? RECENT : recent_used + n;
}

size_t logbuf_recent(char *out, size_t size)
{
    if (!size)
        return 0;
    pthread_mutex_lock(&lock);
    const size_t n = recent_used < size - 1 ? recent_used : size - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = recent[(recent_end + RECENT - n + i) % RECENT];
    pthread_mutex_unlock(&lock);
    out[n] = 0;
    return n;
}

/* Every stdout/stderr write goes through xsdk_log_write_bytes (xsdk src/file.c); -Wl,--wrap sends it here. */
void __real_xsdk_log_write_bytes(const char *bytes, size_t length_bytes);

/* Bytes on to where they go now: the recent ring, and the file or (while a session runs) memory. Under lock. */
static void put(const char *bytes, size_t length_bytes)
{
    keep_recent(bytes, length_bytes);
    if (!buffering) {
        __real_xsdk_log_write_bytes(bytes, length_bytes);
        return;
    }
    if (used + length_bytes <= CAPACITY) {
        memcpy(buffer + used, bytes, length_bytes);
        used += length_bytes;
    } else {
        lost_bytes += (unsigned)length_bytes;
    }
}

/* Each line starts with the console's local date and time (xsdk sets the time zone from XAM), to the millisecond. */
static bool line_start = true;

static void put_stamp(void)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const time_t seconds = now.tv_sec;
    struct tm local;
    localtime_r(&seconds, &local);
    char stamp[32];
    const size_t n = strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    const int more = snprintf(stamp + n, sizeof stamp - n, ".%03ld ", now.tv_nsec / 1000000);
    put(stamp, n + (size_t)(more > 0 ? more : 0));
}

void __wrap_xsdk_log_write_bytes(const char *bytes, size_t length_bytes)
{
    pthread_mutex_lock(&lock);
    if (!logging) {
        pthread_mutex_unlock(&lock);
        return;
    }
    /* Through to each line's end; a stamp before every line that starts here. */
    while (length_bytes) {
        if (line_start)
            put_stamp();
        const char *newline = memchr(bytes, '\n', length_bytes);
        const size_t n = newline ? (size_t)(newline - bytes) + 1 : length_bytes;
        put(bytes, n);
        line_start = newline != NULL;
        bytes += n;
        length_bytes -= n;
    }
    pthread_mutex_unlock(&lock);
}

void logbuf_set_logging(bool on)
{
    pthread_mutex_lock(&lock);
    logging = on;
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

void logbuf_flush(void)
{
    pthread_mutex_lock(&lock);
    const bool was = buffering;
    pthread_mutex_unlock(&lock);
    if (!was)
        return;
    logbuf_end();
    logbuf_begin();
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
