/* The menus' sounds: soft clicks for moving, choosing and going back. They have the console's audio output only
 * while no session runs (a title gets one, and the stream's audio takes it: audio.c), so main.c closes them before a
 * session starts and opens them again once it's back in the menus. */
#pragma once
#include <stdbool.h>

typedef enum {
    UISOUND_MOVE,     /* the cursor moved, or a tab changed */
    UISOUND_CHANGE,   /* a value changed, or a switch flipped */
    UISOUND_SELECT,   /* something was chosen */
    UISOUND_BACK,     /* back, or closed */
    UISOUNDS,
} UiSound;

/* Opens the audio output and its feeder thread; nothing if already open. */
void uisound_open(void);
/* Closes them, waiting for the thread; nothing if closed. */
void uisound_close(void);
/* Starts the sound, cutting off any still playing; nothing while closed. Any thread. */
void uisound_play(UiSound sound);
