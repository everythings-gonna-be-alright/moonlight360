/* App covers for the Play tab, read from the covers folder apps.c fills (no network): decoded on a thread of their
 * own so the menu never waits, kept for a handful of apps. */
#pragma once
#include <stdint.h>

/* The cover of app id on host, APPS_COVER_W x APPS_COVER_H RGBA (apps.h), once it's loaded; NULL until then, or if
 * there's no such file. Asks for it the first time. Valid until the cache drops it: use it to make a texture now. */
const uint8_t *coverart_get(const char *host, int id);
/* Counts the covers loaded: a new number means coverart_get may now give one it didn't. */
unsigned coverart_generation(void);
