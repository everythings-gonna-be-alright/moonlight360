/* Controllers: read with XAM about 500 times a second on a thread of their own, and sent to the host from there
 * while streaming, so a press goes out within about a millisecond instead of waiting for the next screen refresh. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#define INPUT_PADS 4

typedef struct {
    bool connected[INPUT_PADS];
    uint16_t buttons[INPUT_PADS];
    uint16_t pressed[INPUT_PADS];   /* buttons that went down since the last input_poll (none are missed) */
    INPUT_GAMEPAD pad[INPUT_PADS];  /* full state: sticks and triggers too */
} InputState;

/* What the input thread sends to the host. */
typedef enum {
    INPUT_FORWARD_OFF,       /* nothing: no stream */
    INPUT_FORWARD_LIVE,      /* the pads as they are */
    INPUT_FORWARD_NEUTRAL,   /* every pad released, sticks centred: a menu has the controller */
} InputForward;

/* Starts the input thread (once). Without it nothing reads the pads. */
void input_start(void);
/* The pads as the thread last read them, with every button pressed since the previous call. */
void input_poll(InputState *state);
void input_set_forward(InputForward mode);
/* Forgets what was sent (call when a stream starts, before forwarding). */
void input_reset(void);
/* Stops rumble on every pad. */
void input_stop_rumble(void);
/* moonlight-common-c rumble callback. */
void input_rumble(unsigned short controller, unsigned short low, unsigned short high);
