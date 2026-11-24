#include "theme.h"

/* Each accent: the colour itself, a dark shade of it (outlines, soft fills), a light one for text on the dark
 * background, and a faint tint (message boxes). All four read with THEME_ON_ACCENT's near-black on the accent. */
const Theme themes[THEMES] = {
    { "Orange", 0xFFFF7A1A, 0xFF4A2D18, 0xFFFFA45E, 0x22FF7A1A },
    { "Blue", 0xFF3D8BFF, 0xFF1B2B48, 0xFF8DBBFF, 0x223D8BFF },
    { "Violet", 0xFF9D74FF, 0xFF2E2550, 0xFFC6B0FF, 0x229D74FF },
    { "Pink", 0xFFFF5FA8, 0xFF4A1C33, 0xFFFFA3CB, 0x22FF5FA8 },
};

const Theme *theme = &themes[0];

void theme_set(int index)
{
    theme = &themes[index >= 0 && index < THEMES ? index : 0];
}
