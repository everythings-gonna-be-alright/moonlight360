/* Controllers: read with XAM, sent to the host while streaming. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#define INPUT_PADS 4

typedef struct {
    bool connected[INPUT_PADS];
    uint16_t buttons[INPUT_PADS];
    uint16_t pressed[INPUT_PADS];   /* buttons that went down since the last poll */
    INPUT_GAMEPAD pad[INPUT_PADS];  /* full state: sticks and triggers too */
} InputState;

void input_poll(InputState *state);
/* Sends every connected pad's state to the host (only changes are sent). */
void input_send(const InputState *state);
/* Sends every connected pad as released (nothing pressed, sticks centred), e.g. when a menu takes the controller. */
void input_send_neutral(const InputState *state);
/* Forgets what was sent (call when a stream starts). */
void input_reset(void);
/* Stops rumble on every pad. */
void input_stop_rumble(void);
/* moonlight-common-c rumble callback. */
void input_rumble(unsigned short controller, unsigned short low, unsigned short high);
