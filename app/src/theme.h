/* The menus' colours and layout grid (1280x720, inside the TV's title-safe area). */
#pragma once
#include <stdint.h>

/* Colours stay out of enums: a value past INT_MAX would make a whole enum unsigned (xsdk docs/PITFALLS.md). */
static const uint32_t
    THEME_BG = 0xFF0C1016,
    THEME_SURFACE = 0xFF151B24,
    THEME_BORDER = 0xFF222B37,
    THEME_GLOW = 0x384A7BFF,
    THEME_PANEL = 0xE0131C2B,
    THEME_PANEL_EDGE = 0xFF1C273A,
    THEME_CARD_TOP = 0xFF22427A,
    THEME_CARD_BOTTOM = 0xFF17233A,
    THEME_ROW_SELECTED = 0xFF1C2A42,
    THEME_DIVIDER = 0x14FFFFFF,
    THEME_ROW = 0x00000000,
    THEME_SHADOW = 0x90000000,
    THEME_ACCENT = 0xFF4A7BFF,
    THEME_ACCENT_SOFT = 0xFF2C4675,
    THEME_ACCENT_TEXT = 0xFF8EB0FF,
    THEME_TEXT = 0xFFFFFFFF,
    THEME_TEXT_2 = 0xFFA7B3C5,
    THEME_TEXT_3 = 0xFF6B788C,
    THEME_GOOD = 0xFF4CC38A,
    THEME_WARN = 0xFFFFB547,
    THEME_ERROR = 0xFFFF6B6B,
    THEME_TRACK = 0xFF34405A,
    THEME_KNOB_OFF = 0xFFB8C2D0;

enum {
    THEME_LEFT = 64,
    THEME_RIGHT = 1216,
    THEME_HEADER_Y = 34,
    THEME_PANEL_Y = 108,
    THEME_PANEL_BOTTOM = 616,
    THEME_FOOTER_Y = 652,   /* centre line of the button hints */
    THEME_RADIUS = 18,
};
