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
    /* Every pad released; the controller is the PC's mouse and a few keys instead. Right stick: the pointer. Left
     * stick: the wheel. A or RT: left button. X or LT: right button. B: Esc. Start: Enter. LB: Backspace. RB: Tab.
     * D-pad: the arrow keys. The stream menu's own buttons (input_set_menu_buttons) stay the menu's. */
    INPUT_FORWARD_MOUSE,
} InputForward;

/* Keys the stream menu sends to the PC. Guide is the controller's Guide button, which the console keeps to itself:
 * Steam opens Big Picture on it, Windows the Game Bar. */
typedef enum {
    SHORTCUT_ESC,
    SHORTCUT_ENTER,
    SHORTCUT_TAB,
    SHORTCUT_ALT_TAB,
    SHORTCUT_ALT_ENTER,   /* full screen on and off, in most games */
    SHORTCUT_ALT_F4,
    SHORTCUT_WIN,
    SHORTCUT_WIN_D,       /* shows the desktop */
    SHORTCUT_GUIDE,
    SHORTCUTS,
} InputShortcut;

/* Starts the input thread (once). Without it nothing reads the pads. */
void input_start(void);
/* The pads as the thread last read them, with every button pressed since the previous call. */
void input_poll(InputState *state);
void input_set_forward(InputForward mode);
/* Forgets what was sent (call when a stream starts, before forwarding). */
void input_reset(void);
/* Presses and releases a shortcut's keys on the PC, from the input thread, while a stream runs. */
void input_send_shortcut(InputShortcut shortcut);
/* Mouse mode: the buttons that open the stream menu (INPUT_GAMEPAD_ bits), which press no key on the PC. */
void input_set_menu_buttons(uint16_t buttons);
/* Mouse mode's pointer speed: 0 slow, 1 normal, 2 fast. */
void input_set_pointer_speed(int speed);
/* Controller, mouse and key events sent to the host since the app started; rumble commands taken from it; and how
 * many of those were passed on to a pad. */
unsigned input_events_sent(void);
unsigned input_rumbles(void);
unsigned input_rumbles_passed(void);
/* How the host's rumble commands reach the pads: INPUT_RUMBLE_ALL, each at once as it comes; 0, none; or at most
 * that many a second, the newest strengths (see main.c: on Wi-Fi each one passed on risks the console's network). */
enum { INPUT_RUMBLE_ALL = -1 };
void input_set_rumble_limit(int per_second);
/* A test: pad 1 reads as connected with its right stick turning, changed per_second times a second (0 stops it), so
 * a stream carries a player's input traffic with nobody at the controller. */
void input_set_fake(int per_second);
/* A test: per_second rumble commands a second go to the first pad from here, barely felt, as a game's would from the
 * host (0 stops them). */
void input_set_fake_rumble(int per_second);
/* Stops rumble on every pad. */
void input_stop_rumble(void);
/* moonlight-common-c rumble callback. */
void input_rumble(unsigned short controller, unsigned short low, unsigned short high);
