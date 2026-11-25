/* Keeps the log in memory while a stream runs.
 *
 * The log file is on USB storage and each line is a synchronous write (xsdk src/log.c). The console's Wi-Fi adapter
 * is a USB device too. Writing log lines while stream traffic arrives is the one thing every network death had in
 * common (2026-11-22): so during a session lines go to RAM, and reach the file once the session is over. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* false: lines always go straight to the file, as before (for A/B tests). Default true. */
void logbuf_set_enabled(bool enabled);
/* false: every line is dropped, nothing reaches the file or memory (the Other tab's Logging, off by default). */
void logbuf_set_logging(bool on);
/* The newest log text (up to 8 KB, while logging is on) into out, NUL-terminated; returns its length. Any thread. */
size_t logbuf_recent(char *out, size_t size);
/* From now on lines are kept in memory. */
void logbuf_begin(void);
/* Writes what was kept to the file and goes on keeping lines: for when the network has died mid-stream, so the log
 * survives the reboot that follows (and the file's USB writes can no longer be what troubles the network). */
void logbuf_flush(void);
/* Writes what was kept to the file; lines go straight to the file again. */
void logbuf_end(void);
