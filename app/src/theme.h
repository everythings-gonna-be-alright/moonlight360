/* The menus' colours and layout grid (1280x720, inside the TV's title-safe area). */
#pragma once
#include <stdint.h>

/* Colours stay out of enums: a value past INT_MAX would make a whole enum unsigned (xsdk docs/PITFALLS.md). */
/* An accent colour (orange by default; the Theme setting picks another: theme.c) on near-black and neutral grays.
 * Text drawn on THEME_ACCENT uses THEME_ON_ACCENT: white on orange is too faint to read from the couch. THEME_WARN is
 * yellow and THEME_ERROR red, which no theme's accent is, so neither ever looks like a selection. */
enum { THEMES = 4 };
typedef struct {
    const char *name;
    uint32_t accent, soft, text, tint;   /* THEME_ACCENT, _SOFT, _TEXT and _TINT */
} Theme;
extern const Theme themes[THEMES];
extern const Theme *theme;   /* the one in use */
/* Switches the accent colours to themes[index] (the first if it's out of range). Drawing thread. */
void theme_set(int index);
#define THEME_ACCENT (theme->accent)
#define THEME_ACCENT_SOFT (theme->soft)
#define THEME_ACCENT_TEXT (theme->text)
#define THEME_ACCENT_TINT (theme->tint)

static const uint32_t
    THEME_BG = 0xFF0E0E10,
    THEME_SURFACE = 0xFF19191C,
    THEME_BORDER = 0xFF2C2C31,
    THEME_ROW_SELECTED = 0xFF232326,
    THEME_DIVIDER = 0x12FFFFFF,
    THEME_ON_ACCENT = 0xFF121212,
    THEME_TEXT = 0xFFF4F4F5,
    THEME_TEXT_2 = 0xFFB3B3B9,
    THEME_TEXT_3 = 0xFF74747C,
    THEME_GOOD = 0xFF4CC38A,
    THEME_WARN = 0xFFFFD14A,
    THEME_ERROR = 0xFFFF5C5C,
    THEME_ERROR_TINT = 0x24FF5C5C,
    THEME_ERROR_TEXT = 0xFFFFC4C4,
    THEME_TRACK = 0xFF38383E,
    THEME_KNOB_OFF = 0xFFAFAFB6;

enum {
    THEME_LEFT = 64,
    THEME_RIGHT = 1216,
    THEME_HEADER_Y = 34,
    THEME_PANEL_Y = 108,
    THEME_PANEL_BOTTOM = 616,
    THEME_FOOTER_Y = 652,   /* centre line of the button hints */
    THEME_RADIUS = 18,
};
