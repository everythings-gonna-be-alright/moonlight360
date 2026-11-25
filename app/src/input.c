#include "input.h"

#include <math.h>
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

    /* Mouse mode. The pointer and the wheel go out at most this often: a stream shows 60 frames a second at best,
     * and small packets are what the console's Wi-Fi handles badly. */
    MOUSE_INTERVAL_US = 16000,
    MOUSE_DEADZONE = 8000,       /* of 32767: a resting stick reads up to about 6000 */
    MOUSE_SPEED = 1500,          /* the PC's pixels a second with the stick fully over; the square of the tilt below */
    BOOST_AFTER_US = 250000,     /* the stick held fully over this long: the pointer speeds up, to cross the screen */
    BOOST_OVER_US = 600000,      /* ... to twice the speed, over this long */
    WHEEL_SPEED = 1200,          /* wheel units a second fully over: ten notches of 120 */
    WHEEL_STEP = 30,             /* the least sent at once: a quarter notch */
    TRIGGER_PRESSED = 100,       /* of 255 */
    KEY_STEP_US = 30000,         /* between a shortcut's key events, so the PC sees Alt held before Tab */
    GUIDE_HOLD_US = 200000,
    KEY_QUEUE = 16,
};

/* Mouse mode's buttons and keys, as bits. A key held repeats on the PC by itself (Sunshine's key repeat). */
enum { M_LEFT, M_RIGHT, M_UP, M_DOWN, M_KEY_LEFT, M_KEY_RIGHT, M_ENTER, M_ESC, M_BACKSPACE, M_TAB, M_COUNT };
enum { VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_MENU = 0x12, VK_ESCAPE = 0x1B, VK_LEFT = 0x25, VK_UP,
       VK_RIGHT, VK_DOWN, VK_D = 0x44, VK_LWIN = 0x5B, VK_F4 = 0x73 };

typedef struct {
    uint8_t vk;
    bool down;
    uint8_t modifiers;   /* MODIFIER_ flags held with this event */
} KeyStep;

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

/* Shortcuts waiting to go out, under lock. */
static KeyStep key_queue[KEY_QUEUE];
static int key_count;
static uint64_t guide_until_us;   /* the Guide button reads as held until then */

/* Mouse mode, touched by the input thread only (and input_reset). */
static uint16_t mouse_sent;       /* M_ bits down on the PC */
static uint16_t mouse_ignore;     /* M_ bits held when the mode began: nothing until they are let go */
static float mouse_x, mouse_y, wheel;   /* movement not sent yet */
static uint64_t mouse_last_us, mouse_sent_us;
static uint64_t mouse_full_us;    /* how long the stick has been fully over */
static atomic_int menu_buttons;   /* the stream menu's buttons: no keys of mouse mode's */
static atomic_uint events_sent, rumbles;
static atomic_int fake_per_second, fake_rumble_per_second;
/* Rumble (input_set_rumble_limit). What the host last asked of each pad waits here while the limit holds it back;
 * under lock, as is what each pad was last told. */
static atomic_int rumble_limit = INPUT_RUMBLE_ALL;
static atomic_uint rumbles_passed;
static INPUT_VIBRATION rumble_wanted[INPUT_PADS], rumble_told[INPUT_PADS];
static bool rumble_waiting[INPUT_PADS];
static uint64_t rumble_passed_us;   /* the input thread's: when it last passed one on */
static atomic_int pointer_speed = 1;

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

/* Presses and releases what differs from what the PC has. */
static void mouse_apply(uint16_t want)
{
    static const uint8_t keys[M_COUNT] = { [M_UP] = VK_UP,           [M_DOWN] = VK_DOWN,     [M_KEY_LEFT] = VK_LEFT,
                                           [M_KEY_RIGHT] = VK_RIGHT, [M_ENTER] = VK_RETURN,  [M_ESC] = VK_ESCAPE,
                                           [M_BACKSPACE] = VK_BACK,  [M_TAB] = VK_TAB };
    const uint16_t changed = want ^ mouse_sent;
    for (int bit = 0; bit < M_COUNT; bit++) {
        if (!(changed & (1u << bit)))
            continue;
        const bool down = want & (1u << bit);
        if (bit == M_LEFT || bit == M_RIGHT)
            LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE,
                                   bit == M_LEFT ? BUTTON_LEFT : BUTTON_RIGHT);
        else
            LiSendKeyboardEvent((short)(0x8000 | keys[bit]), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, 0);
    }
    mouse_sent = want;
}

/* A stick's tilt past the dead zone, 0 to 1 a side, the dead zone taken off its length (so slow diagonals work). */
static void stick(const InputState *state, bool right, float *x, float *y)
{
    *x = *y = 0;
    float best = 0;
    for (int i = 0; i < INPUT_PADS; i++) {
        if (!state->connected[i])
            continue;
        const float sx = right ? state->pad[i].thumb_rx : state->pad[i].thumb_lx;
        const float sy = right ? state->pad[i].thumb_ry : state->pad[i].thumb_ly;
        const float length = sqrtf(sx * sx + sy * sy);
        if (length <= MOUSE_DEADZONE || length <= best)
            continue;
        best = length;   /* the pad pushed furthest */
        float tilt = (length - MOUSE_DEADZONE) / (32767.0f - MOUSE_DEADZONE);
        tilt = tilt > 1.0f ? 1.0f : tilt;
        *x = sx / length * tilt;
        *y = sy / length * tilt;
    }
}

/* Mouse mode: the pads become the PC's mouse and a few keys. Returns the events sent. */
static unsigned mouse_update(const InputState *state, uint64_t now)
{
    uint16_t buttons = 0;
    uint8_t left_trigger = 0, right_trigger = 0;
    for (int i = 0; i < INPUT_PADS; i++) {
        if (!state->connected[i])
            continue;
        buttons |= state->pad[i].buttons;
        left_trigger = state->pad[i].left_trigger > left_trigger ? state->pad[i].left_trigger : left_trigger;
        right_trigger = state->pad[i].right_trigger > right_trigger ? state->pad[i].right_trigger : right_trigger;
    }
    /* A console's habits: A chooses, B backs out, Start confirms. The menu's buttons stay the menu's. */
    static const struct {
        uint16_t button;
        uint8_t bit;
    } map[] = { { INPUT_GAMEPAD_A, M_LEFT },           { INPUT_GAMEPAD_X, M_RIGHT },
                { INPUT_GAMEPAD_B, M_ESC },            { INPUT_GAMEPAD_START, M_ENTER },
                { INPUT_GAMEPAD_LEFT_SHOULDER, M_BACKSPACE }, { INPUT_GAMEPAD_RIGHT_SHOULDER, M_TAB },
                { INPUT_GAMEPAD_DPAD_UP, M_UP },       { INPUT_GAMEPAD_DPAD_DOWN, M_DOWN },
                { INPUT_GAMEPAD_DPAD_LEFT, M_KEY_LEFT }, { INPUT_GAMEPAD_DPAD_RIGHT, M_KEY_RIGHT } };
    buttons &= (uint16_t)~atomic_load(&menu_buttons);
    uint16_t want = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (buttons & map[i].button)
            want |= (uint16_t)(1u << map[i].bit);
    if (right_trigger > TRIGGER_PRESSED)
        want |= 1u << M_LEFT;
    if (left_trigger > TRIGGER_PRESSED)
        want |= 1u << M_RIGHT;
    /* The A that chose Mouse mode in the stream menu is not a click. */
    mouse_ignore &= want;
    want &= (uint16_t)~mouse_ignore;
    const uint16_t before = mouse_sent;
    mouse_apply(want);
    unsigned sent = (unsigned)__builtin_popcount((unsigned)(before ^ want));

    /* The square of the tilt: slow near the centre to land on a button, fast at the edge to cross the screen, and
     * faster still once the stick has stayed fully over. */
    static const float speeds[] = { 0.6f, 1.0f, 1.6f };
    const uint64_t step_us = now - mouse_last_us > 20000 ? 20000 : now - mouse_last_us;
    const float dt = (float)step_us * 1e-6f;
    mouse_last_us = now;
    float x, y;
    stick(state, true, &x, &y);
    const float tilt = sqrtf(x * x + y * y);
    mouse_full_us = tilt > 0.95f ? mouse_full_us + step_us : 0;
    float boost = mouse_full_us > BOOST_AFTER_US ? (float)(mouse_full_us - BOOST_AFTER_US) / BOOST_OVER_US : 0.0f;
    boost = 1.0f + (boost > 1.0f ? 1.0f : boost);
    const float speed = tilt * MOUSE_SPEED * speeds[atomic_load(&pointer_speed)] * boost * dt;
    mouse_x += x * speed;
    mouse_y -= y * speed;   /* the stick's up is positive, the screen's negative */
    stick(state, false, &x, &y);
    wheel += y * fabsf(y) * WHEEL_SPEED * dt;
    if (now - mouse_sent_us < MOUSE_INTERVAL_US)
        return sent;
    const int dx = (int)mouse_x, dy = (int)mouse_y;
    if (dx || dy) {
        LiSendMouseMoveEvent((short)dx, (short)dy);
        mouse_x -= (float)dx;
        mouse_y -= (float)dy;
        mouse_sent_us = now;
        sent++;
    }
    if (y == 0 && fabsf(wheel) < WHEEL_STEP) {
        wheel = 0;   /* the stick is back: no leftover scrolls later */
    } else if (fabsf(wheel) >= WHEEL_STEP) {
        const int amount = (int)wheel;
        LiSendHighResScrollEvent((short)amount);
        wheel -= (float)amount;
        mouse_sent_us = now;
        sent++;
    }
    return sent;
}

static void mouse_begin(uint64_t now)
{
    mouse_x = mouse_y = wheel = 0;
    mouse_last_us = mouse_sent_us = now;
    mouse_full_us = 0;
    mouse_ignore = 0xFFFF;   /* whatever is held now: mouse_update drops each bit once it is released */
}

/* Sends the next key of a shortcut, if one is due. */
static unsigned send_key_step(uint64_t now)
{
    static uint64_t last_us;
    if (now - last_us < KEY_STEP_US)
        return 0;
    pthread_mutex_lock(&lock);
    const bool have = key_count > 0;
    const KeyStep step = key_queue[0];
    if (have)
        memmove(key_queue, key_queue + 1, (size_t)--key_count * sizeof key_queue[0]);
    pthread_mutex_unlock(&lock);
    if (!have)
        return 0;
    LiSendKeyboardEvent((short)(0x8000 | step.vk), step.down ? KEY_ACTION_DOWN : KEY_ACTION_UP, (char)step.modifiers);
    last_us = now;
    return 1;
}

static void pass_rumble(uint64_t now);

static uint32_t input_thread(void *context)
{
    (void)context;
    xsdk_thread_set_priority(XSDK_CURRENT_THREAD, INPUT_PRIORITY, NULL);
    uint64_t window_start = now_us();
    unsigned reads = 0, sends = 0, counted = 0;   /* counted: how many of sends events_sent already has */
    InputForward was = INPUT_FORWARD_OFF;
    for (;;) {
        InputState state;
        read_pads(&state);
        const uint64_t now = now_us();
        pthread_mutex_lock(&lock);
        for (int i = 0; i < INPUT_PADS; i++)
            pressed_since[i] |= state.buttons[i] & ~latest.buttons[i];
        latest = state;
        const bool guide = now < guide_until_us;
        pthread_mutex_unlock(&lock);
        reads++;

        /* The test's turning stick (input_set_fake), in place of whatever pad 1 reads. */
        const int fake = atomic_load(&fake_per_second);
        if (fake > 0) {
            const uint64_t step = now * (uint64_t)fake / 1000000;
            state.connected[0] = true;
            memset(&state.pad[0], 0, sizeof state.pad[0]);
            state.pad[0].thumb_rx = (int16_t)((step & 1) ? 20000 : -20000);
            state.pad[0].thumb_ry = (int16_t)((step & 2) ? 20000 : -20000);
            state.buttons[0] = 0;
        }
        pass_rumble(now);
        /* The test's rumble commands (input_set_fake_rumble), while a stream runs. */
        const int fake_rumble = atomic_load(&fake_rumble_per_second);
        if (fake_rumble > 0 && atomic_load(&forward) != INPUT_FORWARD_OFF) {
            static uint64_t last_step;
            const uint64_t step = now * (uint64_t)fake_rumble / 1000000;
            if (step != last_step) {
                last_step = step;
                input_rumble(0, (unsigned short)((step & 1) ? 900 : 600), 0);
            }
        }
        const InputForward mode = (InputForward)atomic_load(&forward);
        if (mode != was) {
            if (was == INPUT_FORWARD_MOUSE && mode != INPUT_FORWARD_OFF)
                mouse_apply(0);   /* nothing stays held on the PC */
            if (mode == INPUT_FORWARD_MOUSE)
                mouse_begin(now);
            printf("[I] input: forwarding %s\n", mode == INPUT_FORWARD_LIVE      ? "the pads"
                                                 : mode == INPUT_FORWARD_NEUTRAL ? "released pads (a menu is open)"
                                                 : mode == INPUT_FORWARD_MOUSE   ? "the pads as a mouse"
                                                                                 : "nothing");
            was = mode;
        }
        if (mode != INPUT_FORWARD_OFF) {
            InputState out = state;
            if (mode != INPUT_FORWARD_LIVE) {   /* released pads: only changes go out, once, then nothing */
                memset(&out, 0, sizeof out);
                memcpy(out.connected, state.connected, sizeof out.connected);
            }
            for (int i = 0; guide && i < INPUT_PADS; i++)
                if (out.connected[i]) {   /* the stream menu's Guide shortcut, on the first pad */
                    out.pad[i].buttons |= INPUT_GAMEPAD_GUIDE;
                    break;
                }
            sends += send_changes(&out, now);
            if (mode == INPUT_FORWARD_MOUSE)
                sends += mouse_update(&state, now);
            sends += send_key_step(now);
            atomic_store(&events_sent, atomic_load(&events_sent) + sends - counted);
            counted = sends;
        }
        if (now - window_start >= LOG_EVERY_S * 1000000ULL) {
            counted = 0;
            if (mode != INPUT_FORWARD_OFF)
                printf("[I] input: %.0f reads and %.1f input events a second\n",
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
    mouse_sent = 0;
    pthread_mutex_lock(&lock);
    key_count = 0;
    guide_until_us = 0;
    pthread_mutex_unlock(&lock);
}

void input_set_menu_buttons(uint16_t buttons)
{
    atomic_store(&menu_buttons, (int)buttons);
}

void input_set_pointer_speed(int speed)
{
    atomic_store(&pointer_speed, speed < 0 ? 0 : speed > 2 ? 2 : speed);
}

void input_send_shortcut(InputShortcut shortcut)
{
    static const KeyStep esc[] = { { VK_ESCAPE, true, 0 }, { VK_ESCAPE, false, 0 } };
    static const KeyStep enter[] = { { VK_RETURN, true, 0 }, { VK_RETURN, false, 0 } };
    static const KeyStep alt_tab[] = { { VK_MENU, true, MODIFIER_ALT }, { VK_TAB, true, MODIFIER_ALT },
                                       { VK_TAB, false, MODIFIER_ALT }, { VK_MENU, false, 0 } };
    static const KeyStep alt_f4[] = { { VK_MENU, true, MODIFIER_ALT }, { VK_F4, true, MODIFIER_ALT },
                                      { VK_F4, false, MODIFIER_ALT }, { VK_MENU, false, 0 } };
    static const KeyStep win[] = { { VK_LWIN, true, MODIFIER_META }, { VK_LWIN, false, 0 } };
    static const KeyStep tab[] = { { VK_TAB, true, 0 }, { VK_TAB, false, 0 } };
    static const KeyStep alt_enter[] = { { VK_MENU, true, MODIFIER_ALT }, { VK_RETURN, true, MODIFIER_ALT },
                                         { VK_RETURN, false, MODIFIER_ALT }, { VK_MENU, false, 0 } };
    static const KeyStep win_d[] = { { VK_LWIN, true, MODIFIER_META }, { VK_D, true, MODIFIER_META },
                                     { VK_D, false, MODIFIER_META }, { VK_LWIN, false, 0 } };
    static const struct {
        const KeyStep *steps;
        int count;
    } keys[SHORTCUTS] = { [SHORTCUT_ESC] = { esc, 2 },         [SHORTCUT_ENTER] = { enter, 2 },
                          [SHORTCUT_ALT_TAB] = { alt_tab, 4 }, [SHORTCUT_ALT_F4] = { alt_f4, 4 },
                          [SHORTCUT_WIN] = { win, 2 },         [SHORTCUT_TAB] = { tab, 2 },
                          [SHORTCUT_ALT_ENTER] = { alt_enter, 4 }, [SHORTCUT_WIN_D] = { win_d, 4 } };
    if ((int)shortcut < 0 || shortcut >= SHORTCUTS)
        return;
    pthread_mutex_lock(&lock);
    if (shortcut == SHORTCUT_GUIDE) {
        guide_until_us = now_us() + GUIDE_HOLD_US;
    } else if (key_count + keys[shortcut].count <= KEY_QUEUE) {   /* a whole shortcut or none: no key left down */
        memcpy(key_queue + key_count, keys[shortcut].steps, (size_t)keys[shortcut].count * sizeof key_queue[0]);
        key_count += keys[shortcut].count;
    }
    pthread_mutex_unlock(&lock);
    printf("[I] input: shortcut %d sent to the PC\n", (int)shortcut);
}

unsigned input_events_sent(void)
{
    return atomic_load(&events_sent);
}

unsigned input_rumbles(void)
{
    return atomic_load(&rumbles);
}

void input_set_fake_rumble(int per_second)
{
    atomic_store(&fake_rumble_per_second, per_second);
    if (per_second)
        printf("[I] input: %d synthetic rumble commands a second while a stream runs (test)\n", per_second);
}

void input_set_fake(int per_second)
{
    atomic_store(&fake_per_second, per_second);
}

/* Tells the pad, unless it was told the same already. Returns whether it was told. */
static bool tell_pad(int pad, INPUT_VIBRATION v)
{
    pthread_mutex_lock(&lock);
    const bool same = rumble_told[pad].left_motor_speed == v.left_motor_speed &&
                      rumble_told[pad].right_motor_speed == v.right_motor_speed;
    rumble_told[pad] = v;
    rumble_waiting[pad] = false;
    pthread_mutex_unlock(&lock);
    if (same)
        return false;
    XamInputSetState((uint32_t)pad, 0, &v);
    atomic_fetch_add(&rumbles_passed, 1);
    return true;
}

void input_rumble(unsigned short controller, unsigned short low, unsigned short high)
{
    atomic_fetch_add(&rumbles, 1);
    if (controller >= INPUT_PADS)
        return;
    const INPUT_VIBRATION v = { low, high };
    const int limit = atomic_load(&rumble_limit);
    if (limit == INPUT_RUMBLE_ALL) {
        tell_pad(controller, v);
    } else if (limit > 0) {   /* the input thread passes the newest on when the limit allows */
        pthread_mutex_lock(&lock);
        rumble_wanted[controller] = v;
        rumble_waiting[controller] = true;
        pthread_mutex_unlock(&lock);
    }
}

/* The input thread's part: one waiting command passed on, if the limit's interval has gone by. */
static void pass_rumble(uint64_t now)
{
    const int limit = atomic_load(&rumble_limit);
    if (limit <= 0 || now - rumble_passed_us < 1000000ULL / (uint64_t)limit)
        return;
    static int next;
    for (int i = 0; i < INPUT_PADS; i++) {
        const int pad = (next + i) % INPUT_PADS;
        pthread_mutex_lock(&lock);
        const bool waiting = rumble_waiting[pad];
        const INPUT_VIBRATION v = rumble_wanted[pad];
        pthread_mutex_unlock(&lock);
        if (waiting && tell_pad(pad, v)) {
            rumble_passed_us = now;
            next = pad + 1;
            return;
        }
    }
}

void input_set_rumble_limit(int per_second)
{
    atomic_store(&rumble_limit, per_second);
}

unsigned input_rumbles_passed(void)
{
    return atomic_load(&rumbles_passed);
}

void input_stop_rumble(void)
{
    for (int i = 0; i < INPUT_PADS; i++)
        tell_pad(i, (INPUT_VIBRATION){ 0, 0 });
}
