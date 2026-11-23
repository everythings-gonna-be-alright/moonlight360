#include "overlay.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "gfx.h"
#include "theme.h"

enum {
    ITEM_RESUME,
    ITEM_STATS,
    ITEM_DISCONNECT,
    ITEM_QUIT,
    ITEMS,
    CARD_W = 520,
    CARD_H = 476,
    BUTTON_H = 56,
    BUTTON_GAP = 8,
};

static bool open;
static int selected;
static bool stick_was, stick_x_was;

void overlay_open(void)
{
    open = true;
    selected = ITEM_RESUME;
}

void overlay_close(void)
{
    open = false;
}

bool overlay_is_open(void)
{
    return open;
}

OverlayAction overlay_update(const InputState *input, int *stats_mode)
{
    if (!open)
        return OVERLAY_NONE;
    uint16_t pressed = 0;
    int stick = 0, stick_x = 0;
    for (int pad = 0; pad < INPUT_PADS; pad++) {
        pressed |= input->pressed[pad];
        if (input->connected[pad] && input->pad[pad].thumb_ly > 16000)
            stick = 1;
        if (input->connected[pad] && input->pad[pad].thumb_ly < -16000)
            stick = -1;
        if (input->connected[pad] && input->pad[pad].thumb_lx > 16000)
            stick_x = 1;
        if (input->connected[pad] && input->pad[pad].thumb_lx < -16000)
            stick_x = -1;
    }
    /* The left stick moves too, once per push. */
    if (stick && !stick_was)
        pressed |= stick > 0 ? INPUT_GAMEPAD_DPAD_UP : INPUT_GAMEPAD_DPAD_DOWN;
    stick_was = stick != 0;
    if (stick_x && !stick_x_was)
        pressed |= stick_x > 0 ? INPUT_GAMEPAD_DPAD_RIGHT : INPUT_GAMEPAD_DPAD_LEFT;
    stick_x_was = stick_x != 0;
    /* The Stats row: A or right steps forward, left back; the menu stays open to show the result behind it. */
    if (selected == ITEM_STATS && (pressed & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_DPAD_RIGHT | INPUT_GAMEPAD_DPAD_LEFT))) {
        const int step = (pressed & INPUT_GAMEPAD_DPAD_LEFT) ? STATS_MODES - 1 : 1;
        *stats_mode = (*stats_mode + step) % STATS_MODES;
        return OVERLAY_STATS;
    }
    if (pressed & INPUT_GAMEPAD_DPAD_UP)
        selected = (selected + ITEMS - 1) % ITEMS;
    if (pressed & INPUT_GAMEPAD_DPAD_DOWN)
        selected = (selected + 1) % ITEMS;
    if (pressed & INPUT_GAMEPAD_B) {
        open = false;
        return OVERLAY_RESUME;
    }
    if (pressed & INPUT_GAMEPAD_A) {
        open = false;
        return selected == ITEM_RESUME       ? OVERLAY_RESUME
               : selected == ITEM_DISCONNECT ? OVERLAY_DISCONNECT
                                             : OVERLAY_QUIT_APP;
    }
    return OVERLAY_NONE;
}

void overlay_draw(uint32_t width, uint32_t height, const char *app, int stats_mode)
{
    if (!open)
        return;
    gfx_rect(0, 0, (float)width, (float)height, 0, 0x90000000);   /* light enough to see the stats HUD change */
    const float x = ((float)width - CARD_W) / 2, y = ((float)height - CARD_H) / 2;
    gfx_rect(x, y, CARD_W, CARD_H, THEME_RADIUS, THEME_BORDER);
    gfx_rect(x + 1.0f, y + 1.0f, CARD_W - 2.0f, CARD_H - 2.0f, THEME_RADIUS - 1, THEME_SURFACE);

    const int32_t left = (int32_t)x + 32;
    gfx_text(left, (int32_t)y + 30, 2, THEME_TEXT, "Stream menu");
    char line[96];
    gfx_fit(line, sizeof line, app, 40);
    gfx_text(left, (int32_t)y + 74, 1, THEME_TEXT_3, line);

    char quit[112];
    gfx_fit(line, sizeof line, app, 14);
    snprintf(quit, sizeof quit, "Quit %s", line);
    static const char *const mode_names[STATS_MODES] = { "Off", "Compact", "Detailed" };
    char stats[48];
    snprintf(stats, sizeof stats, "Stats: %s", mode_names[stats_mode]);
    const char *labels[ITEMS] = { "Resume", stats, "Main menu", quit };
    const char *notes[ITEMS] = { "Back to the game.",
                                 "What shows over the game: left and right change it.",
                                 "The app keeps running on the PC: Connect resumes it.",
                                 "Closes the app on the PC, then returns to the main menu." };
    float by = y + 116.0f;
    const float bw = CARD_W - 64.0f;
    for (int i = 0; i < ITEMS; i++, by += BUTTON_H + BUTTON_GAP) {
        const bool on = i == selected;
        if (on)
            gfx_rect((float)left, by, bw, BUTTON_H, 14.0f, i == ITEM_QUIT ? 0xFFC8443C : THEME_ACCENT);
        gfx_text(left + 22, (int32_t)by + (BUTTON_H - 40) / 2, 2,
                 on ? THEME_TEXT : i == ITEM_QUIT ? 0xFFE58A84 : THEME_TEXT_2, labels[i]);
        if (i == ITEM_STATS && on) {   /* arrows: left and right change it */
            gfx_text(left + (int32_t)bw - 48, (int32_t)by + (BUTTON_H - 20) / 2, 1, THEME_TEXT, "\xE2\x97\x80");
            gfx_text(left + (int32_t)bw - 26, (int32_t)by + (BUTTON_H - 20) / 2, 1, THEME_TEXT, "\xE2\x96\xB6");
        }
    }
    char wrapped[160];
    gfx_wrap(wrapped, sizeof wrapped, notes[selected], (CARD_W - 64) / 10);
    gfx_text(left, (int32_t)by + 4, 1, THEME_TEXT_3, wrapped);

    int32_t hx = left;
    const int32_t hy = (int32_t)(y + CARD_H) - 36;
    hx += gfx_hint(hx, hy, 'A', "Choose", THEME_TEXT_2);
    gfx_hint(hx, hy, 'B', "Resume", THEME_TEXT_2);
}
