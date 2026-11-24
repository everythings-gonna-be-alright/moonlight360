/* Fetches the host's app list in the background, for the menu's app picker, then each app's cover: from the
 * covers folder beside the .xex if it was fetched before, else from the host (and kept there). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APPS_MAX = 64,
    /* Covers, fitted to this (3:4, as box art is), red, green, blue and alpha bytes. */
    APPS_COVER_W = 192,
    APPS_COVER_H = 256,
};

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
/* App i's id on the host (0 if there's no such app). */
int apps_id(int i);
/* App i's cover once it's here, else NULL. Valid until the next apps_fetch. */
const uint8_t *apps_cover(int i);
/* Counts fetches: a new number means the list (and covers) were replaced. */
unsigned apps_generation(void);
/* Stops fetching covers (before a stream: its network should have the Wi-Fi to itself). Doesn't wait. */
void apps_stop_covers(void);
