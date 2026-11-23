/* Fetches the host's app list in the background, for the menu's app picker. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

enum { APPS_MAX = 64 };

typedef enum {
    APPS_IDLE,
    APPS_LOADING,
    APPS_READY,
    APPS_FAILED,    /* apps_message says why */
} AppsState;

/* Starts fetching from host (no-op while a fetch runs). */
void apps_fetch(const char *host);
AppsState apps_state(void);
void apps_message(char *out, size_t size);
int apps_count(void);
/* Copies app i's name; running says whether it's the one running on the host now. */
void apps_get(int i, char *name, size_t size, bool *running);
