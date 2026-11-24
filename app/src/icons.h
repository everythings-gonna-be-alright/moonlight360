/* The settings rows' icons: Material Symbols Rounded (Apache License 2.0), drawn smooth from the font's atlas
 * (dev/fonts/gen_font.py's ICONS, in this order). Drawing thread only, like gfx.h. */
#pragma once
#include <stdint.h>

typedef enum {
    ICON_APP,
    ICON_HOST,
    ICON_RESOLUTION,
    ICON_FPS,
    ICON_BITRATE,
    ICON_FAST,
    ICON_LATENCY,
    ICON_STATS,
    ICON_SPEAKER,
    ICON_WAVE,
    ICON_PACKET,
    ICON_SLICES,
    ICON_DECODE,
    ICON_DRAW,
    ICON_LOG,
    ICON_THEME,
    ICON_RECONNECT,
    ICON_HUD_CORNER,
    ICON_RUMBLE,
    ICON_WAKE,
    ICON_TEST,
    ICON_KEYBOARD,
    ICONS,
} Icon;

enum { ICON_SIZE = 30 };   /* as FONT_ICON_PX (font_data.h) */

/* The icon with its top left at (x, y), in argb. */
void icon_draw(Icon icon, float x, float y, uint32_t argb);
