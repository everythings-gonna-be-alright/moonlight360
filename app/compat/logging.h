/* Stand-in for moonlight-tv's commons logging, used by libgamestream. Lines go to the title's log (stdout). */
#pragma once
#include <stddef.h>

typedef enum {
    COMMONS_LOG_LEVEL_VERBOSE,
    COMMONS_LOG_LEVEL_DEBUG,
    COMMONS_LOG_LEVEL_INFO,
    COMMONS_LOG_LEVEL_WARN,
    COMMONS_LOG_LEVEL_ERROR,
    COMMONS_LOG_LEVEL_FATAL,
} commons_log_level;

void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void commons_log_hexdump(commons_log_level level, const char *tag, const void *data, size_t len);

#define commons_log_verbose(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_VERBOSE, tag, __VA_ARGS__)
#define commons_log_debug(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define commons_log_info(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define commons_log_warn(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define commons_log_error(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define commons_log_fatal(tag, ...) commons_log_printf(COMMONS_LOG_LEVEL_FATAL, tag, __VA_ARGS__)
