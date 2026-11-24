/* The stream menu, over the video: Resume, Mouse and keys (a page of its own: mouse mode, text and keys for the
 * PC), the stats HUD's level, Back to the main menu, Quit the app on the PC. Also mouse mode's reminder, drawn while
 * the menu is closed. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "input.h"

typedef enum {
    OVERLAY_NONE,
    OVERLAY_RESUME,
    OVERLAY_DISCONNECT,   /* back to the main menu, the app left running on the PC */
    OVERLAY_QUIT_APP,     /* close the app on the PC, then back to the main menu */
    OVERLAY_STATS,        /* *stats_mode changed (the menu stays open): save it */
    OVERLAY_TYPE,         /* open the on-screen keyboard for overlay_pad(): its text goes to the PC */
    OVERLAY_KEY,          /* send overlay_key() to the PC (the menu stays open) */
    OVERLAY_POINTER,      /* overlay_pointer_speed() changed (the menu stays open): apply and save it */
} OverlayAction;

void overlay_open(void);
void overlay_close(void);
bool overlay_is_open(void);
/* Mouse mode: the controller is the PC's mouse (INPUT_FORWARD_MOUSE). The menu's Mouse mode button switches it. */
bool overlay_mouse(void);
void overlay_set_mouse(bool on);
/* Mouse mode's pointer speed, 0 to POINTER_SPEEDS - 1 (config.pointer_speed): the menu's Pointer speed row. */
int overlay_pointer_speed(void);
void overlay_set_pointer_speed(int speed);
/* The buttons that open this menu and their name ("Back", "LB + RB"...), for mouse mode's reminder. */
void overlay_set_menu_combo(uint16_t buttons, const char *name);
/* The key last pressed in the menu, and the pad that last pressed A there. */
InputShortcut overlay_key(void);
uint32_t overlay_pad(void);
/* stats_mode: config.stats_mode, changed in place by the Stats row. */
OverlayAction overlay_update(const InputState *input, int *stats_mode);
void overlay_draw(uint32_t width, uint32_t height, const char *app, int stats_mode);
