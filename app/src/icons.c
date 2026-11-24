#include "icons.h"

#include "gfx.h"

void icon_draw(Icon icon, float x, float y, uint32_t argb)
{
    if ((unsigned)icon < ICONS)
        gfx_icon((int)icon, x, y, argb);
}
