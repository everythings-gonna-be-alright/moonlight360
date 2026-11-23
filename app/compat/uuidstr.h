/* Stand-in for moonlight-tv's commons uuidstr (same layout), used by libgamestream. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define UUIDSTR_LENGTH 36
#define UUIDSTR_CAPACITY 37

typedef struct uuidstr_t {
    char data[UUIDSTR_LENGTH];
    char zero;
} uuidstr_t;

/* A random (version 4) UUID as text. */
bool uuidstr_random(uuidstr_t *dest);
