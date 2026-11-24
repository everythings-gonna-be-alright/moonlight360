#include "overlay.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "gfx.h"
#include "theme.h"

enum {
    ITEM_RESUME,
    ITEM_PC,
    ITEM_STATS,
    ITEM_DISCONNECT,
    ITEM_QUIT,
    ITEMS,
    /* The Mouse and keys page: three buttons, then a tile a key, KEY_COLUMNS to a row. */
    PC_MOUSE = 0,
    PC_SPEED,
    PC_TYPE,
    PC_KEYS,
    PC_ITEMS = PC_KEYS + SHORTCUTS,
    KEY_COLUMNS = 3,
    KEY_ROWS = (SHORTCUTS + KEY_COLUMNS - 1) / KEY_COLUMNS,
    CARD_W = 520,
    CARD_H = 540,      /* the menu's first page */
    CARD_H_PC = 640,   /* the Mouse and keys page */
    BUTTON_H = 56,
    BUTTON_GAP = 8,
    TILE_H = 48,
    MOUSE_HELP_FRAMES = 600,   /* mouse mode's buttons stay on screen this long (ten seconds), then only its name */
    SENT_FRAMES = 90,          /* "Sent" stays under the keys this long */
};

static bool open;
static bool pc_page;                /* the Mouse and keys page, not the menu's first */
static int selected, pc_selected;
static int key_column;              /* the keys' column last on: up and down return to it */
static bool stick_was, stick_x_was;
static bool mouse_on;
static uint32_t mouse_help_frame;   /* gfx_frame() when mouse mode's help last began */
static int key = SHORTCUT_ESC;      /* the key last sent */
static uint32_t key_sent_frame;
static bool key_sent;
static uint32_t last_pad;
static int pointer_speed = 1;
static uint16_t menu_buttons = INPUT_GAMEPAD_BACK;   /* what opens this menu, and its name */
static const char *menu_name = "Back";

void overlay_open(void)
{
    open = true;
    pc_page = false;
    selected = ITEM_RESUME;
    key_sent = false;
}

void overlay_close(void)
{
    open = false;
}

bool overlay_is_open(void)
{
    return open;
}

bool overlay_mouse(void)
{
    return mouse_on;
}

void overlay_set_mouse(bool on)
{
    mouse_on = on;
    mouse_help_frame = gfx_frame();
}

int overlay_pointer_speed(void)
{
    return pointer_speed;
}

void overlay_set_pointer_speed(int speed)
{
    pointer_speed = speed < 0 || speed >= POINTER_SPEEDS ? 1 : speed;
}

void overlay_set_menu_combo(uint16_t buttons, const char *name)
{
    menu_buttons = buttons;
    menu_name = name;
}

InputShortcut overlay_key(void)
{
    return (InputShortcut)key;
}

uint32_t overlay_pad(void)
{
    return last_pad;
}

/* Back to the stream; mouse mode's help shows again. */
static OverlayAction resume(void)
{
    open = false;
    mouse_help_frame = gfx_frame();
    return OVERLAY_RESUME;
}

/* The Mouse and keys page: Mouse mode, Pointer speed, Type text, then the keys in rows. */
static OverlayAction update_pc(uint16_t pressed)
{
    const bool on_key = pc_selected >= PC_KEYS;
    const int index = pc_selected - PC_KEYS;
    if (on_key)
        key_column = index % KEY_COLUMNS;
    if (pressed & INPUT_GAMEPAD_DPAD_UP) {
        if (on_key)
            pc_selected = index >= KEY_COLUMNS ? pc_selected - KEY_COLUMNS : PC_TYPE;
        else if (pc_selected > PC_MOUSE)
            pc_selected--;
        else
            pc_selected = PC_KEYS + (KEY_ROWS - 1) * KEY_COLUMNS + key_column;   /* round to the last row */
    }
    if (pressed & INPUT_GAMEPAD_DPAD_DOWN) {
        if (on_key)
            pc_selected = index + KEY_COLUMNS < SHORTCUTS ? pc_selected + KEY_COLUMNS : PC_MOUSE;
        else if (pc_selected < PC_TYPE)
            pc_selected++;
        else
            pc_selected = PC_KEYS + key_column;
    }
    if (pc_selected >= PC_ITEMS)
        pc_selected = PC_ITEMS - 1;
    const uint16_t sideways = pressed & (INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_RIGHT);
    if (on_key && pc_selected >= PC_KEYS && sideways) {
        const int now = pc_selected - PC_KEYS, row = now / KEY_COLUMNS;
        const int column = (now % KEY_COLUMNS + ((pressed & INPUT_GAMEPAD_DPAD_LEFT) ? KEY_COLUMNS - 1 : 1)) % KEY_COLUMNS;
        if (row * KEY_COLUMNS + column < SHORTCUTS)
            pc_selected = PC_KEYS + row * KEY_COLUMNS + column;
    }
    /* Pointer speed: A or right steps forward, left back; main.c saves it. */
    if (pc_selected == PC_SPEED && (sideways || (pressed & INPUT_GAMEPAD_A))) {
        pointer_speed = (pointer_speed + ((pressed & INPUT_GAMEPAD_DPAD_LEFT) ? POINTER_SPEEDS - 1 : 1)) % POINTER_SPEEDS;
        return OVERLAY_POINTER;
    }
    if (pressed & INPUT_GAMEPAD_B) {   /* back to the menu's first page */
        pc_page = false;
        return OVERLAY_NONE;
    }
    if (!(pressed & INPUT_GAMEPAD_A))
        return OVERLAY_NONE;
    if (pc_selected == PC_MOUSE) {
        mouse_on = !mouse_on;
        return resume();   /* straight back to the stream, to use it */
    }
    if (pc_selected == PC_TYPE) {
        resume();
        return OVERLAY_TYPE;
    }
    /* A key: pressed on the PC; the menu stays open to press it again, or another. */
    key = pc_selected - PC_KEYS;
    key_sent = true;
    key_sent_frame = gfx_frame();
    return OVERLAY_KEY;
}

OverlayAction overlay_update(const InputState *input, int *stats_mode)
{
    if (!open)
        return OVERLAY_NONE;
    uint16_t pressed = 0;
    int stick = 0, stick_x = 0;
    for (int pad = 0; pad < INPUT_PADS; pad++) {
        pressed |= input->pressed[pad];
        if (input->pressed[pad] & INPUT_GAMEPAD_A)
            last_pad = (uint32_t)pad;
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
    if (pc_page)
        return update_pc(pressed);
    /* From the first page, without going through Mouse and keys: X switches mouse mode, Y opens the keyboard. */
    if (pressed & INPUT_GAMEPAD_X) {
        mouse_on = !mouse_on;
        return resume();
    }
    if (pressed & INPUT_GAMEPAD_Y) {
        for (int pad = 0; pad < INPUT_PADS; pad++)
            if (input->pressed[pad] & INPUT_GAMEPAD_Y)
                last_pad = (uint32_t)pad;
        resume();
        return OVERLAY_TYPE;
    }
    /* The Stats row: A or right steps forward, left back; the menu stays open to show the result behind it. */
    if (selected == ITEM_STATS && (pressed & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_DPAD_RIGHT | INPUT_GAMEPAD_DPAD_LEFT))) {
        const int step = (pressed & INPUT_GAMEPAD_DPAD_LEFT) ? STATS_MODES - 1 : 1;
        *stats_mode = (*stats_mode + step) % STATS_MODES;
        return OVERLAY_STATS;
    }
    if (selected == ITEM_PC && (pressed & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_DPAD_RIGHT))) {
        pc_page = true;
        pc_selected = PC_MOUSE;
        key_sent = false;
        return OVERLAY_NONE;
    }
    if (pressed & INPUT_GAMEPAD_DPAD_UP)
        selected = (selected + ITEMS - 1) % ITEMS;
    if (pressed & INPUT_GAMEPAD_DPAD_DOWN)
        selected = (selected + 1) % ITEMS;
    if (pressed & INPUT_GAMEPAD_B)
        return resume();
    if (pressed & INPUT_GAMEPAD_A) {
        if (selected == ITEM_RESUME)
            return resume();
        open = false;
        return selected == ITEM_DISCONNECT ? OVERLAY_DISCONNECT : OVERLAY_QUIT_APP;
    }
    return OVERLAY_NONE;
}

/* Mouse mode's reminder at the bottom of the picture: what everything does at first, then just that it is on and how
 * to reach the menu (the game ignoring the controller would otherwise look like a fault). */
static void draw_mouse_help(uint32_t width, uint32_t height)
{
    static const struct {
        char button;
        uint16_t bit;   /* left out while it is one of the menu's own buttons */
        const char *label;
    } hints[] = { { 'A', 0, "Click" },
                  { 'X', 0, "Right click" },
                  { 'B', 0, "Esc" },
                  { 'Y', 0, "Keyboard" },
                  { 'S', INPUT_GAMEPAD_START, "Enter" },
                  { 'L', INPUT_GAMEPAD_LEFT_SHOULDER, "Backspace" },
                  { 'R', INPUT_GAMEPAD_RIGHT_SHOULDER, "Tab" } };
    enum { HINTS = sizeof hints / sizeof hints[0], PAD = 22, ROW_H = 38, BOTTOM = 28 };
    char menu[48];
    snprintf(menu, sizeof menu, "%s: menu", menu_name);
    if (gfx_frame() - mouse_help_frame >= MOUSE_HELP_FRAMES) {
        char line[80];
        snprintf(line, sizeof line, "Mouse mode  \xC2\xB7  %s", menu);
        const int32_t w = gfx_text_width(line, 1) + 2 * PAD, x = ((int32_t)width - w) / 2;
        const int32_t y = (int32_t)height - ROW_H - BOTTOM;
        gfx_rect((float)x, (float)y, (float)w, ROW_H, ROW_H / 2.0f, 0x80101014);
        gfx_text(x + PAD, y + ROW_H / 2 - 10, 1, THEME_TEXT, line);
        return;
    }
    char sticks[160];
    snprintf(sticks, sizeof sticks,
             "Right stick: pointer  \xC2\xB7  Left stick: scroll  \xC2\xB7  D-pad: arrow keys  \xC2\xB7  %s", menu);
    /* The buttons' widths, as gfx_hint draws them: the letters are 28 wide, the others as wide as their names. */
    int32_t row = -28;
    for (int i = 0; i < HINTS; i++) {
        if (hints[i].bit & menu_buttons)
            continue;
        const char b = hints[i].button;
        const int32_t glyph = b == 'S' ? gfx_text_width("START", 1) + 18 : b == 'L' || b == 'R' ? gfx_text_width("LB", 1) + 18 : 28;
        row += glyph + 8 + gfx_text_width(hints[i].label, 1) + 28;
    }
    const int32_t text_w = gfx_text_width(sticks, 1);
    const int32_t w = (row > text_w ? row : text_w) + 2 * PAD, h = 2 * ROW_H + 12;
    const int32_t x = ((int32_t)width - w) / 2, y = (int32_t)height - h - BOTTOM;
    gfx_rect((float)x, (float)y, (float)w, (float)h, 20.0f, 0xD0101014);
    int32_t hx = x + (w - row) / 2;
    for (int i = 0; i < HINTS; i++)
        if (!(hints[i].bit & menu_buttons))
            hx += gfx_hint(hx, y + 6 + ROW_H / 2, hints[i].button, hints[i].label, THEME_TEXT);
    gfx_text_center(x + w / 2, y + 6 + ROW_H + ROW_H / 2 - 10, 1, THEME_TEXT_2, sticks);
}

/* The highlight slides to the selected button (about 120 ms); it starts there when the menu opens. */
static void draw_highlight(float x, float y, float w, float h, uint32_t argb)
{
    static float at[4];
    static uint32_t frame;
    const float target[4] = { x, y, w, h };
    const bool jump = gfx_frame() - frame > 2;
    for (int i = 0; i < 4; i++) {
        const float d = target[i] - at[i];
        at[i] = jump || (d > -0.5f && d < 0.5f) ? target[i] : at[i] + d * 0.33f;
    }
    frame = gfx_frame();
    gfx_rect(at[0], at[1], at[2], at[3], 14.0f, argb);
}

static void draw_note(int32_t left, int32_t y, const char *note)
{
    char wrapped[256];
    gfx_wrap(wrapped, sizeof wrapped, note, (CARD_W - 64) / 10);
    gfx_text(left, y, 1, THEME_TEXT_3, wrapped);
}

static void draw_pc(int32_t left, float y)
{
    static const char *const key_names[SHORTCUTS] = { "Esc",     "Enter",   "Tab",     "Alt + Tab", "Alt + Enter",
                                                      "Alt + F4", "Windows", "Win + D", "Guide" };
    static const char *const key_notes[SHORTCUTS] = {
        "A presses the key on the PC.",
        "A presses the key on the PC.",
        "A presses the key on the PC: the next field or button.",
        "Switches to the window used before.",
        "Full screen on and off, in most games.",
        "Closes the window in front.",
        "Opens the Start menu.",
        "Shows the desktop; again brings the windows back.",
        "The controller's Guide button: Steam opens Big Picture on it.",
    };
    static const char *const speed_names[POINTER_SPEEDS] = { "Slow", "Normal", "Fast" };
    const float bw = CARD_W - 64.0f, tile_w = (bw - (KEY_COLUMNS - 1) * BUTTON_GAP) / KEY_COLUMNS;
    const float keys_y = y + 116.0f + PC_KEYS * (BUTTON_H + BUTTON_GAP) + 30.0f;
    float sx = (float)left, sy = y + 116.0f + (float)pc_selected * (BUTTON_H + BUTTON_GAP), sw = bw, sh = BUTTON_H;
    if (pc_selected >= PC_KEYS) {
        const int index = pc_selected - PC_KEYS;
        sx = (float)left + (float)(index % KEY_COLUMNS) * (tile_w + BUTTON_GAP);
        sy = keys_y + (float)(index / KEY_COLUMNS) * (TILE_H + BUTTON_GAP);
        sw = tile_w;
        sh = TILE_H;
    }
    /* Every key is a tile, so they read as buttons before the highlight reaches them. */
    for (int i = 0; i < SHORTCUTS; i++)
        gfx_rect((float)left + (float)(i % KEY_COLUMNS) * (tile_w + BUTTON_GAP),
                 keys_y + (float)(i / KEY_COLUMNS) * (TILE_H + BUTTON_GAP), tile_w, TILE_H, 14.0f, THEME_ROW_SELECTED);
    draw_highlight(sx, sy, sw, sh, THEME_ACCENT);

    const char *labels[PC_KEYS] = { "Mouse mode", "Pointer speed", "Type text" };
    for (int i = 0; i < PC_KEYS; i++) {
        const int32_t by = (int32_t)(y + 116.0f) + i * (BUTTON_H + BUTTON_GAP);
        const int32_t right = left + (int32_t)bw, value_y = by + (BUTTON_H - 20) / 2;
        const bool on = i == pc_selected;
        gfx_text(left + 22, by + (BUTTON_H - 40) / 2, 2, on ? THEME_ON_ACCENT : THEME_TEXT_2, labels[i]);
        if (i == PC_MOUSE)
            gfx_text_right(right - 22, value_y, 1, on ? THEME_ON_ACCENT : mouse_on ? THEME_ACCENT_TEXT : THEME_TEXT_3,
                           mouse_on ? "On" : "Off");
        if (i == PC_SPEED && on) {   /* arrows: left and right change it */
            gfx_text(right - 48, value_y, 1, THEME_ON_ACCENT, "\xE2\x97\x80");
            gfx_text(right - 26, value_y, 1, THEME_ON_ACCENT, "\xE2\x96\xB6");
        }
        if (i == PC_SPEED)
            gfx_text_right(right - (on ? 60 : 22), value_y, 1, on ? THEME_ON_ACCENT : THEME_TEXT_3,
                           speed_names[pointer_speed]);
    }
    gfx_text(left, (int32_t)keys_y - 26, 1, THEME_TEXT_3, "Press a key on the PC");
    for (int i = 0; i < SHORTCUTS; i++) {
        const float tx = (float)left + (float)(i % KEY_COLUMNS) * (tile_w + BUTTON_GAP);
        const int32_t ty = (int32_t)keys_y + (i / KEY_COLUMNS) * (TILE_H + BUTTON_GAP);
        gfx_text_center((int32_t)(tx + tile_w / 2), ty + (TILE_H - 20) / 2, 1,
                        PC_KEYS + i == pc_selected ? THEME_ON_ACCENT : THEME_TEXT, key_names[i]);
    }

    const int32_t note_y = (int32_t)keys_y + KEY_ROWS * (TILE_H + BUTTON_GAP) + 4;
    char sent[64];
    snprintf(sent, sizeof sent, "%s pressed on the PC.", key_names[key]);
    if (pc_selected >= PC_KEYS && key_sent && gfx_frame() - key_sent_frame < SENT_FRAMES)
        gfx_text(left, note_y, 1, THEME_GOOD, sent);
    else
        draw_note(left, note_y,
                  pc_selected == PC_MOUSE   ? "The controller becomes the PC's mouse: the right stick moves the "
                                              "pointer, A clicks. X here in the menu switches it too."
                  : pc_selected == PC_SPEED ? "How fast the right stick moves the pointer: left and right change it."
                  : pc_selected == PC_TYPE  ? "Opens the keyboard: what you type goes to the PC, where its cursor is."
                                            : key_notes[pc_selected - PC_KEYS]);
}

void overlay_draw(uint32_t width, uint32_t height, const char *app, int stats_mode)
{
    if (!open) {
        if (mouse_on)
            draw_mouse_help(width, height);
        return;
    }
    gfx_rect(0, 0, (float)width, (float)height, 0, 0x90000000);   /* light enough to see the stats HUD change */
    const float card_h = pc_page ? CARD_H_PC : CARD_H;
    const float x = ((float)width - CARD_W) / 2, y = ((float)height - card_h) / 2;
    gfx_rect(x, y, CARD_W, card_h, THEME_RADIUS, THEME_BORDER);
    gfx_rect(x + 1.0f, y + 1.0f, CARD_W - 2.0f, card_h - 2.0f, THEME_RADIUS - 1, THEME_SURFACE);

    const int32_t left = (int32_t)x + 32;
    gfx_text(left, (int32_t)y + 30, 2, THEME_TEXT, pc_page ? "Mouse and keys" : "Stream menu");
    char line[96];
    gfx_fit(line, sizeof line, app, 40);
    gfx_text(left, (int32_t)y + 74, 1, THEME_ACCENT_TEXT, line);

    int32_t hx = left;
    const int32_t hy = (int32_t)(y + card_h) - 36;
    hx += gfx_hint(hx, hy, 'A', "Choose", THEME_TEXT_2);
    hx += gfx_hint(hx, hy, 'B', pc_page ? "Back" : "Resume", THEME_TEXT_2);
    if (!pc_page) {   /* the two things wanted most often, a press away */
        hx += gfx_hint(hx, hy, 'X', "Mouse", mouse_on ? THEME_ACCENT_TEXT : THEME_TEXT_2);
        gfx_hint(hx, hy, 'Y', "Type", THEME_TEXT_2);
    }
    if (pc_page) {
        draw_pc(left, y);
        return;
    }

    char quit[112];
    gfx_fit(line, sizeof line, app, 14);
    snprintf(quit, sizeof quit, "Quit %s", line);
    static const char *const mode_names[STATS_MODES] = { "Off", "Compact", "Detailed" };
    char stats[48];
    snprintf(stats, sizeof stats, "Stats: %s", mode_names[stats_mode]);
    const char *labels[ITEMS] = { "Resume", "Mouse and keys", stats, "Main menu", quit };
    const char *notes[ITEMS] = { "Back to the game.",
                                 "Use the controller as the PC's mouse, type text, or press Esc, Alt + Tab and other "
                                 "keys.",
                                 "What shows over the game: left and right change it.",
                                 "The app keeps running on the PC: Connect resumes it.",
                                 "Closes the app on the PC, then returns to the main menu." };
    float by = y + 116.0f;
    const float bw = CARD_W - 64.0f;
    draw_highlight((float)left, by + (float)selected * (BUTTON_H + BUTTON_GAP), bw, BUTTON_H,
                   selected == ITEM_QUIT ? 0xFFC8443C : THEME_ACCENT);
    for (int i = 0; i < ITEMS; i++, by += BUTTON_H + BUTTON_GAP) {
        const bool on = i == selected;
        const uint32_t ink = i == ITEM_QUIT ? THEME_TEXT : THEME_ON_ACCENT;   /* white on red, black on orange */
        const int32_t right = left + (int32_t)bw, arrow_y = (int32_t)by + (BUTTON_H - 20) / 2;
        gfx_text(left + 22, (int32_t)by + (BUTTON_H - 40) / 2, 2,
                 on ? ink : i == ITEM_QUIT ? 0xFFFF8A80 : THEME_TEXT_2, labels[i]);
        if (i == ITEM_STATS && on) {   /* arrows: left and right change it */
            gfx_text(right - 48, arrow_y, 1, ink, "\xE2\x97\x80");
            gfx_text(right - 26, arrow_y, 1, ink, "\xE2\x96\xB6");
        }
        if (i == ITEM_PC) {   /* it opens a page; mouse mode shows here while it is on */
            gfx_text(right - 26, arrow_y, 1, on ? ink : THEME_TEXT_3, "\xE2\x96\xB6");
            if (mouse_on)
                gfx_text_right(right - 40, arrow_y, 1, on ? ink : THEME_ACCENT_TEXT, "Mouse on");
        }
    }
    draw_note(left, (int32_t)by + 4, notes[selected]);
}
