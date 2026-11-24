#include "input.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <Limelight.h>

#include "xsdk/thread.h"

enum {
    POLL_MS = 2,   /* the pad reports about every 8 ms: 2 ms adds at most 1 ms and wakes half as often */
    /* Sticks and triggers alone are sent at most this often: faster than the pad reports them (about every 8 ms), and
     * a cap on small packets, which the console's Wi-Fi handles badly. Buttons go out at once. */
    ANALOG_INTERVAL_US = 4000,
    INPUT_HW_THREAD = 1,
    INPUT_PRIORITY = 2,   /* as Moonlight's network threads: above the decode threads, so they can't hold it up */
    LOG_EVERY_S = 30,
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static InputState latest;                   /* under lock */
static uint16_t pressed_since[INPUT_PADS];  /* under lock: presses not yet handed to input_poll */
static atomic_int forward = INPUT_FORWARD_OFF;
static bool started;

/* What was sent, touched by the input thread only (and input_reset, while forwarding is off). */
static INPUT_GAMEPAD last_sent[INPUT_PADS];
static bool sent_once[INPUT_PADS];
static uint64_t last_analog_us[INPUT_PADS];
static uint16_t previous_mask;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static void read_pads(InputState *state)
{
    for (int i = 0; i < INPUT_PADS; i++) {
        INPUT_STATE now = { 0 };
        state->connected[i] = XamInputGetState(i, 0, &now) == ERROR_SUCCESS;
        if (!state->connected[i])
            memset(&now, 0, sizeof now);
        state->pad[i] = now.gamepad;
        state->buttons[i] = now.gamepad.buttons;
        state->pressed[i] = 0;
    }
}

/* Sends what changed; returns how many controller events went out. */
static unsigned send_changes(const InputState *state, uint64_t now)
{
    unsigned sent = 0;
    uint16_t mask = 0;
    for (int i = 0; i < INPUT_PADS; i++)
        if (state->connected[i])
            mask |= 1u << i;
    for (int i = 0; i < INPUT_PADS; i++) {
        const bool was = previous_mask & (1u << i), is = mask & (1u << i);
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
        /* Only sticks or triggers moved: wait for the interval (the newest values go out then). */
        if (sent_once[i] && g->buttons == last_sent[i].buttons && now - last_analog_us[i] < ANALOG_INTERVAL_US)
            continue;
        LiSendMultiControllerEvent(i, mask, g->buttons, g->left_trigger, g->right_trigger,
                                   g->thumb_lx, g->thumb_ly, g->thumb_rx, g->thumb_ry);
        last_sent[i] = *g;
        sent_once[i] = true;
        last_analog_us[i] = now;
        sent++;
    }
    return sent;
}

static uint32_t input_thread(void *context)
{
    (void)context;
    xsdk_thread_set_priority(XSDK_CURRENT_THREAD, INPUT_PRIORITY, NULL);
    uint64_t window_start = now_us();
    unsigned reads = 0, sends = 0;
    InputForward was = INPUT_FORWARD_OFF;
    for (;;) {
        InputState state;
        read_pads(&state);
        const uint64_t now = now_us();
        pthread_mutex_lock(&lock);
        for (int i = 0; i < INPUT_PADS; i++)
            pressed_since[i] |= state.buttons[i] & ~latest.buttons[i];
        latest = state;
        pthread_mutex_unlock(&lock);
        reads++;

        const InputForward mode = (InputForward)atomic_load(&forward);
        if (mode == INPUT_FORWARD_LIVE) {
            sends += send_changes(&state, now);
        } else if (mode == INPUT_FORWARD_NEUTRAL) {
            InputState neutral;
            memset(&neutral, 0, sizeof neutral);
            memcpy(neutral.connected, state.connected, sizeof neutral.connected);
            sends += send_changes(&neutral, now);   /* only changes go out: once, then nothing */
        }
        if (mode != was) {
            printf("[I] input: forwarding %s\n", mode == INPUT_FORWARD_LIVE      ? "the pads"
                                                 : mode == INPUT_FORWARD_NEUTRAL ? "released pads (a menu is open)"
                                                                                 : "nothing");
            was = mode;
        }
        if (now - window_start >= LOG_EVERY_S * 1000000ULL) {
            if (mode != INPUT_FORWARD_OFF)
                printf("[I] input: %.0f reads and %.1f controller events a second\n",
                       reads * 1e6 / (double)(now - window_start), sends * 1e6 / (double)(now - window_start));
            window_start = now;
            reads = sends = 0;
        }
        xsdk_thread_sleep(POLL_MS);
    }
    return 0;
}

void input_start(void)
{
    if (started)
        return;
    pthread_mutex_lock(&lock);
    read_pads(&latest);
    pthread_mutex_unlock(&lock);
    HANDLE h;
    if (!NT_SUCCESS(xsdk_thread_create_on(&h, 16 * 1024, input_thread, NULL, INPUT_HW_THREAD))) {
        printf("[E] input: the input thread didn't start\n");
        return;
    }
    started = true;
}

void input_poll(InputState *state)
{
    pthread_mutex_lock(&lock);
    *state = latest;
    for (int i = 0; i < INPUT_PADS; i++) {
        state->pressed[i] = pressed_since[i];
        pressed_since[i] = 0;
    }
    pthread_mutex_unlock(&lock);
}

void input_set_forward(InputForward mode)
{
    atomic_store(&forward, (int)mode);
}

void input_reset(void)
{
    memset(sent_once, 0, sizeof sent_once);
    memset(last_analog_us, 0, sizeof last_analog_us);
    previous_mask = 0;
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
