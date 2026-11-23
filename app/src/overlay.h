/* The stream menu, over the video: Resume, Back to the main menu, Quit the app on the PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "input.h"

typedef enum {
    OVERLAY_NONE,
    OVERLAY_RESUME,
    OVERLAY_DISCONNECT,   /* back to the main menu, the app left running on the PC */
    OVERLAY_QUIT_APP,     /* close the app on the PC, then back to the main menu */
} OverlayAction;

void overlay_open(void);
void overlay_close(void);
bool overlay_is_open(void);
OverlayAction overlay_update(const InputState *input);
void overlay_draw(uint32_t width, uint32_t height, const char *app);
