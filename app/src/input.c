#include "input.h"

#include <string.h>

#include <Limelight.h>

static INPUT_GAMEPAD last_sent[INPUT_PADS];
static bool sent_once[INPUT_PADS];
static uint16_t previous_mask;
static uint16_t previous_buttons[INPUT_PADS];

void input_poll(InputState *state)
{
    for (int i = 0; i < INPUT_PADS; i++) {
        INPUT_STATE now = { 0 };
        state->connected[i] = XamInputGetState(i, 0, &now) == ERROR_SUCCESS;
        if (!state->connected[i])
            memset(&now, 0, sizeof now);
        state->pad[i] = now.gamepad;
        state->buttons[i] = now.gamepad.buttons;
        state->pressed[i] = now.gamepad.buttons & ~previous_buttons[i];
        previous_buttons[i] = now.gamepad.buttons;
    }
}

void input_reset(void)
{
    memset(sent_once, 0, sizeof sent_once);
    previous_mask = 0;
}

void input_send(const InputState *state)
{
    uint16_t mask = 0;
    for (int i = 0; i < INPUT_PADS; i++)
        if (state->connected[i])
            mask |= 1u << i;

    for (int i = 0; i < INPUT_PADS; i++) {
        bool was = previous_mask & (1u << i), is = mask & (1u << i);
        if (is && !was) {
            /* The 360 pad's button bits are Moonlight's flags, Guide = SPECIAL_FLAG. */
            LiSendControllerArrivalEvent(i, mask, LI_CTYPE_XBOX,
                A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG | RB_FLAG |
                PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG,
                LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);
            sent_once[i] = false;
        } else if (!is && was) {
            LiSendMultiControllerEvent(i, mask, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    previous_mask = mask;

    for (int i = 0; i < INPUT_PADS; i++) {
        if (!state->connected[i])
            continue;
        const INPUT_GAMEPAD *g = &state->pad[i];
        if (sent_once[i] && memcmp(g, &last_sent[i], sizeof *g) == 0)
            continue;
        LiSendMultiControllerEvent(i, mask, g->buttons, g->left_trigger, g->right_trigger,
                                   g->thumb_lx, g->thumb_ly, g->thumb_rx, g->thumb_ry);
        last_sent[i] = *g;
        sent_once[i] = true;
    }
}

void input_rumble(unsigned short controller, unsigned short low, unsigned short high)
{
    if (controller >= INPUT_PADS)
        return;
    INPUT_VIBRATION v = { low, high };
    XamInputSetState(controller, 0, &v);
}

void input_stop_rumble(void)
{
    for (int i = 0; i < INPUT_PADS; i++)
        input_rumble(i, 0, 0);
}

void input_send_neutral(const InputState *state)
{
    InputState neutral;
    memset(&neutral, 0, sizeof neutral);
    memcpy(neutral.connected, state->connected, sizeof neutral.connected);
    input_send(&neutral);
}
