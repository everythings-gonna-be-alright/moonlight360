#include "overlay.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "gfx.h"
#include "theme.h"
#include "ui.h"

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
static uint32_t open_frame;   /* gfx_frame() when the menu opened: Fresh's panel slides in */

/* Fresh's first page: five rows. The mouse, typing and the PC's keys are on the Mouse and keyboard page. */
enum { FR_RESUME, FR_MORE, FR_STATS, FR_LEAVE, FR_QUIT, FR_ROWS, QUIT_HOLD_FRAMES = 60 };
static int fr_row;
static bool quit_holding;          /* A held on Quit: the game closes when it has been held QUIT_HOLD_FRAMES */
static uint32_t quit_hold_from;

void overlay_open(void)
{
    open = true;
    open_frame = gfx_frame();
    fr_row = FR_RESUME;
    quit_holding = false;
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

/* Fresh's first page. Up and down stop at the ends (Resume can't wrap round to Quit); B, or the button that opened the
 * menu, resumes; Quit wants A held for a second. Only A and B: the mouse and typing are on their page. */
static OverlayAction update_fresh(const InputState *input, uint16_t pressed, int *stats_mode)
{
    uint16_t held = 0;
    for (int pad = 0; pad < INPUT_PADS; pad++)
        held |= input->buttons[pad];
    if (quit_holding) {
        if (fr_row != FR_QUIT || !(held & INPUT_GAMEPAD_A)) {
            quit_holding = false;
        } else if (gfx_frame() - quit_hold_from >= QUIT_HOLD_FRAMES) {
            quit_holding = false;
            open = false;
            return OVERLAY_QUIT_APP;
        }
        return OVERLAY_NONE;
    }
    if ((pressed & INPUT_GAMEPAD_B) || (pressed & menu_buttons & (INPUT_GAMEPAD_BACK | INPUT_GAMEPAD_START)))
        return resume();
    if ((pressed & INPUT_GAMEPAD_DPAD_UP) && fr_row > 0)
        fr_row--;
    if ((pressed & INPUT_GAMEPAD_DPAD_DOWN) && fr_row < FR_ROWS - 1)
        fr_row++;
    if (fr_row == FR_STATS && (pressed & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_DPAD_RIGHT | INPUT_GAMEPAD_DPAD_LEFT))) {
        const int step = (pressed & INPUT_GAMEPAD_DPAD_LEFT) ? STATS_MODES - 1 : 1;
        *stats_mode = (*stats_mode + step) % STATS_MODES;
        return OVERLAY_STATS;
    }
    if (fr_row == FR_MORE && (pressed & INPUT_GAMEPAD_DPAD_RIGHT)) {
        pc_page = true;
        pc_selected = PC_MOUSE;
        key_sent = false;
        return OVERLAY_NONE;
    }
    if (!(pressed & INPUT_GAMEPAD_A))
        return OVERLAY_NONE;
    for (int pad = 0; pad < INPUT_PADS; pad++)
        if (input->pressed[pad] & INPUT_GAMEPAD_A)
            last_pad = (uint32_t)pad;
    switch (fr_row) {
    case FR_RESUME:
        return resume();
    case FR_MORE:
        pc_page = true;
        pc_selected = PC_MOUSE;
        key_sent = false;
        return OVERLAY_NONE;
    case FR_LEAVE:
        open = false;
        return OVERLAY_DISCONNECT;
    case FR_QUIT:
        quit_holding = true;
        quit_hold_from = gfx_frame();
        return OVERLAY_NONE;
    default:
        return OVERLAY_NONE;
    }
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
    if (ui_fresh())
        return update_fresh(input, pressed, stats_mode);
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

/* Fresh: a panel in the middle of the screen over the dimmed game: the app, then Resume, Mouse and keys, Stats,
 * Leave and Quit in a column; the selected one's note under them, and the buttons at the panel's foot. The Mouse and
 * keys page is the same panel (draw_fresh_pc). */
enum { F_W = 440, F_PAD = 24, F_ROW = 56, F_GAP = 6, F_SLIDE = 12 };
static const uint32_t F_SURFACE = 0xFF141416, F_SELECTED = 0xFF2A2A2E, F_EDGE = 0x8CF4F4F5, F_TEXT = 0xFFF4F4F5,
                      F_TEXT_2 = 0xFFB3B3B9, F_TEXT_3 = 0xFF8B8B93, F_QUIT = 0xFFFF6B6B, F_LINE = 0xFF232326,
                      F_TILE = 0xFF1E1E21;

/* The dimmed game and the panel, h tall, rising and fading in as the menu opens; returns its top left. */
static void fresh_panel(uint32_t width, uint32_t height, float h, float *px, float *py)
{
    const uint32_t age = gfx_frame() - open_frame;
    const float in = age >= F_SLIDE ? 1.0f : (float)age / F_SLIDE, ease = 1.0f - (1.0f - in) * (1.0f - in);
    gfx_rect(0.0f, 0.0f, (float)width, (float)height, 0.0f, (uint32_t)(ease * 150.0f) << 24);
    const float x = ((float)width - F_W) / 2.0f, y = ((float)height - h) / 2.0f + 24.0f * (1.0f - ease);
    gfx_shadow(x, y + 24.0f, F_W, h, 26.0f, 60.0f, (uint32_t)(ease * 150.0f) << 24);
    gfx_rect(x, y, F_W, h, 26.0f, F_SURFACE);
    *px = x;
    *py = y;
}

/* A row: the white button for Resume, a lighter surface with a thin edge for the others (red edged for Quit). */
static uint32_t fresh_row(float x, float y, float w, bool on, bool primary, bool danger)
{
    if (on && primary) {
        gfx_shadow(x, y + 8.0f, w, F_ROW, F_ROW / 2.0f, 24.0f, 0x66000000);
        gfx_rect(x, y, w, F_ROW, F_ROW / 2.0f, 0xFFF4F4F5);
        return 0xFF121212;
    }
    if (on) {
        gfx_rect(x - 2.0f, y - 2.0f, w + 4.0f, F_ROW + 4.0f, 18.0f, danger ? 0x8CFF6B6B : F_EDGE);
        gfx_rect(x, y, w, F_ROW, 16.0f, danger ? 0xFF2E1A1A : F_SELECTED);
    }
    return danger ? F_QUIT : on ? F_TEXT : F_TEXT_2;
}

static void fresh_small(int32_t x, int32_t y, uint32_t argb, const char *text)
{
    gfx_scale_begin(0.8f, (float)x, (float)y);
    gfx_text(x, y, 1, argb, text);
    gfx_scale_end();
}

static void draw_fresh_pc(uint32_t width, uint32_t height, const char *app)
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
    enum { TILE = 50 };
    const float h = 70.0f + PC_KEYS * (F_ROW + F_GAP) + 34.0f + KEY_ROWS * (TILE + F_GAP) + 50.0f + 64.0f;
    float x, y;
    fresh_panel(width, height, h, &x, &y);
    const int32_t left = (int32_t)x + F_PAD;
    (void)app;
    gfx_text(left + 18, (int32_t)y + F_PAD + 2, 1, F_TEXT_3, "Mouse and keyboard");   /* where B goes back from */

    const float rw = F_W - 2.0f * F_PAD;
    static const char *const labels[PC_KEYS] = { "Mouse mode", "Pointer speed", "Type text" };
    float ry = y + 70.0f;
    for (int i = 0; i < PC_KEYS; i++, ry += F_ROW + F_GAP) {
        const bool on = i == pc_selected;
        const uint32_t ink = fresh_row((float)left, ry, rw, on, false, false);
        const int32_t ty = (int32_t)ry + (F_ROW - 20) / 2, right = left + (int32_t)rw - 18;
        gfx_text(left + 18, ty, 1, ink, labels[i]);
        if (i == PC_MOUSE)
            gfx_text_right(right, ty, 1, mouse_on ? THEME_ACCENT : F_TEXT_3, mouse_on ? "On" : "Off");
        if (i == PC_SPEED) {
            int32_t vr = right;
            if (on) {
                gfx_text_right(vr, ty, 1, F_TEXT_3, "\xE2\x96\xB6");
                vr -= gfx_text_width("\xE2\x96\xB6", 1) + 12;
            }
            gfx_text_right(vr, ty, 1, on ? F_TEXT : F_TEXT_2, speed_names[pointer_speed]);
            if (on)
                gfx_text_right(vr - gfx_text_width(speed_names[pointer_speed], 1) - 12, ty, 1, F_TEXT_3,
                               "\xE2\x97\x80");
        }
    }
    fresh_small(left + 4, (int32_t)ry + 8, F_TEXT_3, "Press a key on the PC");
    const float keys_y = ry + 34.0f, tile_w = (rw - (KEY_COLUMNS - 1) * F_GAP) / KEY_COLUMNS;
    for (int i = 0; i < SHORTCUTS; i++) {
        const float tx = (float)left + (float)(i % KEY_COLUMNS) * (tile_w + F_GAP);
        const float ty = keys_y + (float)(i / KEY_COLUMNS) * (TILE + F_GAP);
        const bool on = PC_KEYS + i == pc_selected;
        if (on)
            gfx_rect(tx - 2.0f, ty - 2.0f, tile_w + 4.0f, TILE + 4.0f, 16.0f, F_EDGE);
        gfx_rect(tx, ty, tile_w, TILE, 14.0f, on ? F_SELECTED : F_TILE);
        gfx_text_center((int32_t)(tx + tile_w / 2), (int32_t)ty + (TILE - 20) / 2, 1, on ? F_TEXT : F_TEXT_2,
                        key_names[i]);
    }

    /* The selected one's note, or that the key went to the PC. */
    const int32_t note_y = (int32_t)(keys_y + KEY_ROWS * (TILE + F_GAP)) + 6;
    char wrapped[256];
    if (pc_selected >= PC_KEYS && key_sent && gfx_frame() - key_sent_frame < SENT_FRAMES) {
        snprintf(wrapped, sizeof wrapped, "%s pressed on the PC.", key_names[key]);
        fresh_small(left + 4, note_y, 0xFF4CC38A, wrapped);
    } else {
        gfx_wrap(wrapped, sizeof wrapped,
                 pc_selected == PC_MOUSE   ? "The controller becomes the PC's mouse: the right stick moves the "
                                             "pointer, A clicks."
                 : pc_selected == PC_SPEED ? "How fast the right stick moves the pointer: left and right change it."
                 : pc_selected == PC_TYPE  ? "Opens the keyboard: what you type goes to the PC, where its cursor is."
                                           : key_notes[pc_selected - PC_KEYS],
                 48);
        fresh_small(left + 4, note_y, F_TEXT_3, wrapped);
    }
    const int32_t hy = (int32_t)(y + h) - 34;
    gfx_rect((float)left, (float)hy - 30.0f, rw, 1.0f, 0.0f, F_LINE);
    int32_t hx = left;
    hx += gfx_hint(hx, hy, 'A', "Choose", F_TEXT_2) + 4;
    gfx_hint(hx, hy, 'B', "Back", F_TEXT_2);
}

static void draw_fresh(uint32_t width, uint32_t height, const char *app, int stats_mode)
{
    /* No heading: the game is right behind it. */
    enum { LEAVE_H = F_ROW + 10 };
    const float h = F_PAD + 3 * (F_ROW + F_GAP) + 17.0f + (LEAVE_H + F_GAP) + F_ROW + 50.0f + 64.0f;
    float x, y;
    fresh_panel(width, height, h, &x, &y);
    const int32_t left = (int32_t)x + F_PAD;
    const float rw = F_W - 2.0f * F_PAD;
    static const char *const mode_names[STATS_MODES] = { "Off", "Compact", "Detailed" };
    float ry = y + F_PAD;
    for (int r = FR_RESUME; r <= FR_STATS; r++, ry += F_ROW + F_GAP) {
        const bool on = fr_row == r;
        const uint32_t ink = fresh_row((float)left, ry, rw, on, r == FR_RESUME, false);
        const int32_t ty = (int32_t)ry + (F_ROW - 20) / 2, right = left + (int32_t)rw - 18;
        static const char *const labels[FR_ROWS] = { "Resume", "Mouse and keyboard", "Stats", NULL, NULL };
        gfx_text(left + 18, ty, 1, ink, labels[r]);
        if (r == FR_MORE) {
            gfx_text_right(right, ty, 1, on ? F_TEXT : F_TEXT_3, "\xE2\x80\xBA");
            if (mouse_on)   /* while it's on, say so here */
                gfx_text_right(right - 22, ty, 1, THEME_ACCENT, "Mouse on");
        } else if (r == FR_STATS) {
            int32_t vr = right;
            if (on) {
                gfx_text_right(vr, ty, 1, F_TEXT_3, "\xE2\x96\xB6");
                vr -= gfx_text_width("\xE2\x96\xB6", 1) + 12;
            }
            gfx_text_right(vr, ty, 1, on ? F_TEXT : F_TEXT_2, mode_names[stats_mode]);
            if (on)
                gfx_text_right(vr - gfx_text_width(mode_names[stats_mode], 1) - 12, ty, 1, F_TEXT_3,
                               "\xE2\x97\x80");
        }
    }

    /* The two ways out, after a line. Leave: back to the menus, the game left running, said under its name. */
    gfx_rect((float)left + 18.0f, ry + 2.0f, rw - 36.0f, 1.0f, 0.0f, F_LINE);
    ry += 11.0f;
    {
        const bool on = fr_row == FR_LEAVE;
        if (on) {
            gfx_rect((float)left - 2.0f, ry - 2.0f, rw + 4.0f, LEAVE_H + 4.0f, 18.0f, F_EDGE);
            gfx_rect((float)left, ry, rw, LEAVE_H, 16.0f, F_SELECTED);
        }
        gfx_text(left + 18, (int32_t)ry + 10, 1, on ? F_TEXT : F_TEXT_2, "Leave stream");
        fresh_small(left + 18, (int32_t)ry + 36, on ? F_TEXT_2 : F_TEXT_3, "The game keeps running on the PC");
        ry += LEAVE_H + F_GAP;
    }
    /* Quit: A held fills it, and the game closes when it's full. */
    const uint32_t ink = fresh_row((float)left, ry, rw, fr_row == FR_QUIT, false, true);
    if (quit_holding) {
        const float k = (float)(gfx_frame() - quit_hold_from) / QUIT_HOLD_FRAMES;
        gfx_rect((float)left, ry, rw * (k > 1.0f ? 1.0f : k), F_ROW, 16.0f, 0x66FF5C5C);
    }
    char line[112], quit[128];
    gfx_fit_width(line, sizeof line, app, 1, F_W - 2 * F_PAD - 110);
    snprintf(quit, sizeof quit, "Quit %s", line);
    gfx_text(left + 18, (int32_t)ry + (F_ROW - 20) / 2, 1, ink, quit);
    ry += F_ROW + F_GAP;

    static const char *const notes[FR_ROWS] = {
        "Back to the game. B does it too, or the button that opened this menu.",
        "The controller as the PC's mouse, typing, and keys like Esc and Alt + Tab.",
        "What shows over the game: left and right change it.",
        "Back to Moonlight's menus. Start streaming picks the game up where you left it.",
        "Hold A for a second: closes the game on the PC.",
    };
    char wrapped[256];
    gfx_wrap(wrapped, sizeof wrapped, notes[fr_row], 50);
    fresh_small(left + 4, (int32_t)ry + 6, F_TEXT_3, wrapped);

    const int32_t hy = (int32_t)(y + h) - 34;
    gfx_rect((float)left, (float)hy - 30.0f, rw, 1.0f, 0.0f, F_LINE);
    int32_t hx = left;
    hx += gfx_hint(hx, hy, 'A', fr_row == FR_QUIT ? "Hold to quit" : "Choose", F_TEXT_2) + 4;
    gfx_hint(hx, hy, 'B', "Resume", F_TEXT_2);
}

void overlay_draw(uint32_t width, uint32_t height, const char *app, int stats_mode)
{
    if (!open) {
        if (mouse_on)
            draw_mouse_help(width, height);
        return;
    }
    if (ui_fresh()) {
        if (pc_page)
            draw_fresh_pc(width, height, app);
        else
            draw_fresh(width, height, app, stats_mode);
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
