/* The connection flow, on a thread of its own: keys, server status, pairing, app launch, stream. */
#pragma once
#include <stdbool.h>

#include "config.h"

/* Client certificate, key and unique ID, beside the .xex. */
#define SESSION_KEY_DIR "moonlight-keys"

typedef enum {
    SESSION_IDLE,
    SESSION_WORKING,    /* text says what it's doing */
    SESSION_PAIRING,    /* text holds the PIN to enter on the host */
    SESSION_STREAMING,
    SESSION_FAILED,     /* text holds the error */
    SESSION_ENDED,      /* the stream ended; text says why */
} SessionState;

/* Where a connection has got to, for the progress screen. */
typedef enum {
    SESSION_STEP_REACH,       /* keys, then the host's status */
    SESSION_STEP_PAIR,        /* skipped (done at once) if already paired */
    SESSION_STEP_START_APP,   /* find the app, close another, launch */
    SESSION_STEP_STREAM,      /* moonlight-common-c's connection stages */
    SESSION_STEP_PICTURE,     /* streaming, waiting for the first frame (main.c) */
    SESSION_STEPS,
} SessionStep;

typedef struct {
    SessionState state;
    SessionStep step;
    char text[512];
    char pin[8];        /* while pairing: the PIN to enter on the host */
    char mac[18];       /* the PC's MAC address as it reported it, once it has (main.c keeps it for Wake-on-LAN) */
    int app_id;         /* the host's id for the app, once found (main.c keeps it, for its cover) */
} SessionView;

void session_start(const Config *config);
/* Ends a stream (or waits for the flow to give up) and joins the thread. Blocks. */
void session_stop(void);
/* Asks a stream to end without blocking; quit_app also closes the app on the host. The session then goes to
 * SESSION_WORKING while it winds down, and SESSION_ENDED once done. */
void session_request_stop(bool quit_app);
void session_get_view(SessionView *view);
