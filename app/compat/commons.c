/* Implementations of the commons stand-ins (logging.h, uuidstr.h). */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "logging.h"
#include "uuidstr.h"

/* Debug and verbose lines are dropped unless MOONLIGHT_LOG_DEBUG is defined. */
void commons_log_printf(commons_log_level level, const char *tag, const char *fmt, ...)
{
#ifndef MOONLIGHT_LOG_DEBUG
    if (level < COMMONS_LOG_LEVEL_INFO)
        return;
#endif
    static const char *const names[] = { "V", "D", "I", "W", "E", "F" };
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    printf("[%s] %s: %s\n", names[level], tag, line);
}

void commons_log_hexdump(commons_log_level level, const char *tag, const void *data, size_t len)
{
    (void)level, (void)tag, (void)data, (void)len;
}

bool uuidstr_random(uuidstr_t *dest)
{
    unsigned char b[16];
    if (getentropy(b, sizeof b) != 0)
        return false;
    b[6] = (b[6] & 0x0F) | 0x40; /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80; /* RFC 4122 variant */
    snprintf(dest->data, UUIDSTR_CAPACITY,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    dest->zero = '\0';
    return true;
}
