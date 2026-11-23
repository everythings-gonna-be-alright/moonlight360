#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "gfx.h"
#include "perf.h"
#include "theme.h"
#include "ui.h"
#include "xsdk/xam.h"

/* Every setting, by id. */
enum {
    ITEM_CONNECT,
    ITEM_HOST,
    ITEM_APP,
    ITEM_EXIT,
    ITEM_RESOLUTION,
    ITEM_FPS,
    ITEM_BITRATE,
    ITEM_FAST_DECODE,
    ITEM_GPU_DECODE,
    ITEM_STATS,
    ITEM_AUDIO_ON_HOST,
    ITEM_AUDIO_PACKETS,
    ITEM_PACKET_SIZE,
    ITEM_DECODE,
    ITEM_DRAW,
    ITEMS,
};

enum {
    TAB_PLAY,
    TAB_VIDEO,
    TAB_NETWORK,
    TAB_DEBUG,
    TABS,
    TAB_ITEMS_MAX = 6,
};

enum {
    KEYBOARD_CHARS = 64,
    MIN_BITRATE_KBPS = 1000,
    MAX_BITRATE_KBPS = 30000,
    BITRATE_STEP_KBPS = 1000,
    /* Held D-pad or stick: repeats after REPEAT_DELAY frames, then every REPEAT_RATE. */
    REPEAT_DELAY = 24,
    REPEAT_RATE = 5,
    STICK_THRESHOLD = 16000,
    /* Layout. */
    CONTENT_X = 80,
    CONTENT_RIGHT = 1200,
    CONTENT_Y = 136,
    ROW_H = 62,
    HELP_CHARS = (CONTENT_RIGHT - CONTENT_X - 32) / 10,
    PICKER_VISIBLE = 6,
    PICKER_Y = 196,
};

typedef struct {
    const char *name;
    int count;
    int items[TAB_ITEMS_MAX];
} Tab;

static const Tab tabs[TABS] = {
    { "Play", 3, { ITEM_CONNECT, ITEM_APP, ITEM_HOST } },
    { "Video", 6, { ITEM_RESOLUTION, ITEM_FPS, ITEM_BITRATE, ITEM_FAST_DECODE, ITEM_GPU_DECODE, ITEM_STATS } },
    { "Network", 3, { ITEM_AUDIO_ON_HOST, ITEM_AUDIO_PACKETS, ITEM_PACKET_SIZE } },
    { "Debug", 2, { ITEM_DECODE, ITEM_DRAW } },
};

typedef struct {
    int width, height;
    const char *name;
} Resolution;

/* 720p is the most the console decodes in real time (the benchmark: 17-28 ms a frame). */
static const Resolution resolutions[] = {
    { 640, 360, "640\xC3\x97" "360" },
    { 854, 480, "854\xC3\x97" "480" },
    { 960, 540, "960\xC3\x97" "540" },
    { 1280, 720, "1280\xC3\x97" "720" },
    { 1600, 900, "1600\xC3\x97" "900" },   /* 60 fps at high bitrates: 31% fewer pixels than 1080p */
    { 1920, 1080, "1920\xC3\x97" "1080" },
};
static const int frame_rates[] = { 30, 60 };
static const int audio_durations[] = { 20, 10, 5 };
/* No 512/256: smaller packets mean more packets per second, and a flood of small packets (5 ms audio, or 256-byte
 * video at ~4000/s) makes the console's Wi-Fi stop receiving until reboot (2026-11-22). */
static const int packet_sizes[] = { 1392, 1024 };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ARROW_LEFT "\xE2\x97\x80"    /* U+25C0 */
#define ARROW_RIGHT "\xE2\x96\xB6"   /* U+25B6 */

static Config *config;
static const char *config_path;
static int tab;
static int cursor[TABS];
static char message[512];

/* The on-screen keyboard, while it's open. */
static XAM_OVERLAPPED keyboard_overlapped;
static uint16_t keyboard_text[KEYBOARD_CHARS + 1];
static uint16_t keyboard_title[48], keyboard_description[96], keyboard_default[KEYBOARD_CHARS + 1];
static int keyboard_item = -1;

/* The app picker, while it's open. */
static bool picker_open;
static int picker_selected, picker_top;

/* D-pad and stick repeat. */
static uint16_t nav_held;
static uint32_t nav_next;

static int selected_item(void)
{
    return tabs[tab].items[cursor[tab]];
}

static void select_item(int item)
{
    for (int t = 0; t < TABS; t++)
        for (int i = 0; i < tabs[t].count; i++)
            if (tabs[t].items[i] == item) {
                tab = t;
                cursor[t] = i;
            }
}

static void to_utf16(uint16_t *out, size_t capacity, const char *in)
{
    size_t i = 0;
    for (; in[i] && i + 1 < capacity; i++)
        out[i] = (unsigned char)in[i];
    out[i] = 0;
}

/* Keeps ASCII; anything else becomes '?'. Host names, IP addresses and app names are ASCII in practice. */
static void from_utf16(char *out, size_t capacity, const uint16_t *in)
{
    size_t i = 0;
    for (; in[i] && i + 1 < capacity; i++)
        out[i] = in[i] < 0x80 ? (char)in[i] : '?';
    out[i] = 0;
}

static void save(void)
{
    config_save(config_path, config);
}

void menu_open(Config *c, const char *path)
{
    config = c;
    config_path = path;
    select_item(config->host[0] ? ITEM_CONNECT : ITEM_HOST);
}

void menu_set_message(const char *text)
{
    snprintf(message, sizeof message, "%s", text ? text : "");
    if (message[0])
        tab = TAB_PLAY;   /* where the message shows */
}

static int resolution_index(void)
{
    for (int i = 0; i < COUNT(resolutions); i++)
        if (resolutions[i].width == config->width && resolutions[i].height == config->height)
            return i;
    return -1;
}

static int index_of(const int *values, int count, int value)
{
    for (int i = 0; i < count; i++)
        if (values[i] == value)
            return i;
    return -1;
}

/* The next of count choices from i (unknown: the first). */
static int step_choice(int i, int count, int step)
{
    return i < 0 ? 0 : (i + step + count) % count;
}

static bool is_toggle(int item)
{
    return item == ITEM_FAST_DECODE || item == ITEM_GPU_DECODE || item == ITEM_STATS || item == ITEM_DECODE ||
           item == ITEM_DRAW;
}

static void change(int item, int step)
{
    switch (item) {
    case ITEM_RESOLUTION: {
        int i = resolution_index();
        i = i < 0 ? 3 : step_choice(i, COUNT(resolutions), step);
        config->width = resolutions[i].width;
        config->height = resolutions[i].height;
        break;
    }
    case ITEM_FPS:
        config->fps = frame_rates[step_choice(index_of(frame_rates, COUNT(frame_rates), config->fps),
                                              COUNT(frame_rates), step)];
        break;
    case ITEM_BITRATE:
        config->bitrate_kbps += step * BITRATE_STEP_KBPS;
        if (config->bitrate_kbps < MIN_BITRATE_KBPS)
            config->bitrate_kbps = MIN_BITRATE_KBPS;
        if (config->bitrate_kbps > MAX_BITRATE_KBPS)
            config->bitrate_kbps = MAX_BITRATE_KBPS;
        break;
    case ITEM_STATS:
        config->show_stats = !config->show_stats;
        break;
    case ITEM_FAST_DECODE:
        config->fast_decode = !config->fast_decode;
        break;
    case ITEM_GPU_DECODE:
        config->gpu_decode = !config->gpu_decode;
        break;
    case ITEM_AUDIO_ON_HOST:
        config->audio_on_host = !config->audio_on_host;
        break;
    case ITEM_AUDIO_PACKETS:
        config->audio_packet_ms = audio_durations[step_choice(
            index_of(audio_durations, COUNT(audio_durations), config->audio_packet_ms), COUNT(audio_durations), step)];
        break;
    case ITEM_PACKET_SIZE:
        config->packet_size = packet_sizes[step_choice(
            index_of(packet_sizes, COUNT(packet_sizes), config->packet_size), COUNT(packet_sizes), step)];
        break;
    case ITEM_DECODE:
        config->decode = !config->decode;
        break;
    case ITEM_DRAW:
        config->draw = !config->draw;
        break;
    default:
        return;
    }
    save();
}

/* ---- the on-screen keyboard ---- */

static void open_keyboard(int item, uint32_t user)
{
    const char *title = item == ITEM_HOST ? "Host PC" : "App";
    const char *description = item == ITEM_HOST ? "IP address of the PC running Sunshine, e.g. 192.168.1.100"
                                                : "Name of the Sunshine app to start, e.g. Desktop";
    to_utf16(keyboard_title, sizeof keyboard_title / 2, title);
    to_utf16(keyboard_description, sizeof keyboard_description / 2, description);
    to_utf16(keyboard_default, sizeof keyboard_default / 2, item == ITEM_HOST ? config->host : config->app);
    memset(keyboard_text, 0, sizeof keyboard_text);
    uint32_t ret = xsdk_xam_keyboard(user, keyboard_title, keyboard_description, keyboard_default, keyboard_text,
                                     KEYBOARD_CHARS, &keyboard_overlapped);
    if (ret == ERROR_IO_PENDING) {
        keyboard_item = item;
    } else {
        printf("[W] menu: keyboard did not open: 0x%08X\n", (unsigned)ret);
        menu_set_message("The on-screen keyboard did not open; try again.");
    }
}

/* Polls the open keyboard; true while it's still open. */
static bool poll_keyboard(void)
{
    if (keyboard_item < 0)
        return false;
    uint32_t ret = xsdk_xam_wait(&keyboard_overlapped, 0, NULL);
    if (ret == ERROR_IO_INCOMPLETE)
        return true;
    if (ret == ERROR_SUCCESS) {
        char text[KEYBOARD_CHARS + 1];
        from_utf16(text, sizeof text, keyboard_text);
        /* Trim spaces the keyboard may leave. */
        char *s = text;
        while (*s == ' ')
            s++;
        size_t n = strlen(s);
        while (n && s[n - 1] == ' ')
            s[--n] = 0;
        if (keyboard_item == ITEM_HOST) {
            snprintf(config->host, sizeof config->host, "%s", s);
            if (config->host[0])
                select_item(ITEM_CONNECT);
        } else {
            snprintf(config->app, sizeof config->app, "%s", s[0] ? s : "Desktop");
            select_item(ITEM_CONNECT);
        }
        save();
    }
    keyboard_item = -1;
    return false;
}

/* ---- input ---- */

/* D-pad and left stick directions that fire this frame: on press, then repeating while held. */
static uint16_t navigation(const InputState *input)
{
    const uint16_t dpad = INPUT_GAMEPAD_DPAD_UP | INPUT_GAMEPAD_DPAD_DOWN | INPUT_GAMEPAD_DPAD_LEFT |
                          INPUT_GAMEPAD_DPAD_RIGHT;
    uint16_t held = 0;
    for (int p = 0; p < INPUT_PADS; p++) {
        held |= input->buttons[p] & dpad;
        held |= input->pressed[p] & dpad;   /* a tap shorter than a frame */
        if (!input->connected[p])
            continue;
        const INPUT_GAMEPAD *g = &input->pad[p];
        if (g->thumb_ly > STICK_THRESHOLD)
            held |= INPUT_GAMEPAD_DPAD_UP;
        if (g->thumb_ly < -STICK_THRESHOLD)
            held |= INPUT_GAMEPAD_DPAD_DOWN;
        if (g->thumb_lx < -STICK_THRESHOLD)
            held |= INPUT_GAMEPAD_DPAD_LEFT;
        if (g->thumb_lx > STICK_THRESHOLD)
            held |= INPUT_GAMEPAD_DPAD_RIGHT;
    }
    const uint32_t now = gfx_frame();
    uint16_t fire = held & ~nav_held;
    if (held != nav_held)
        nav_next = now + REPEAT_DELAY;
    else if (held && (int32_t)(now - nav_next) >= 0) {
        fire |= held;
        nav_next = now + REPEAT_RATE;
    }
    nav_held = held;
    return fire;
}

/* Buttons pressed this frame on any pad, and the first pad that pressed one (for the keyboard's user). */
static uint16_t pressed_buttons(const InputState *input, uint32_t *pad)
{
    uint16_t all = 0;
    *pad = 0;
    for (uint32_t p = 0; p < INPUT_PADS; p++) {
        if (input->pressed[p] && !all)
            *pad = p;
        all |= input->pressed[p];
    }
    return all;
}

/* ---- the app picker ---- */

static void open_picker(void)
{
    picker_open = true;
    picker_selected = picker_top = 0;
    apps_fetch(config->host);
}

static void update_picker(const InputState *input)
{
    uint32_t pad;
    const uint16_t p = pressed_buttons(input, &pad);
    const uint16_t nav = navigation(input);
    AppsState st = apps_state();
    int n = apps_count();
    if (p & INPUT_GAMEPAD_B) {
        picker_open = false;
        return;
    }
    if (p & INPUT_GAMEPAD_X) {
        picker_open = false;
        open_keyboard(ITEM_APP, pad);
        return;
    }
    if ((p & INPUT_GAMEPAD_Y) && st != APPS_LOADING)
        apps_fetch(config->host);
    if (st != APPS_READY || n == 0)
        return;
    if (nav & INPUT_GAMEPAD_DPAD_UP)
        picker_selected = (picker_selected + n - 1) % n;
    if (nav & INPUT_GAMEPAD_DPAD_DOWN)
        picker_selected = (picker_selected + 1) % n;
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
        bool running;
        char name[64];
        apps_get(picker_selected, name, sizeof name, &running);
        snprintf(config->app, sizeof config->app, "%s", name);
        save();
        picker_open = false;
        select_item(ITEM_CONNECT);
        return;
    }
    if (picker_selected < picker_top)
        picker_top = picker_selected;
    if (picker_selected >= picker_top + PICKER_VISIBLE)
        picker_top = picker_selected - PICKER_VISIBLE + 1;
}

/* ---- the menu ---- */

static MenuAction activate(int item, uint32_t pad)
{
    switch (item) {
    case ITEM_CONNECT:
        if (!config->host[0]) {
            open_keyboard(ITEM_HOST, pad);
            return MENU_NONE;
        }
        return MENU_CONNECT;
    case ITEM_HOST:
        open_keyboard(ITEM_HOST, pad);
        return MENU_NONE;
    case ITEM_APP:
        if (!config->host[0]) {
            menu_set_message("Set the host PC's IP address first.");
            select_item(ITEM_HOST);
            return MENU_NONE;
        }
        open_picker();
        return MENU_NONE;
    case ITEM_EXIT:
        return MENU_EXIT;
    default:
        change(item, 1);
        return MENU_NONE;
    }
}

MenuAction menu_update(const InputState *input)
{
    if (poll_keyboard())
        return MENU_NONE;
    if (picker_open) {
        update_picker(input);
        return MENU_NONE;
    }
    uint32_t pad;
    const uint16_t p = pressed_buttons(input, &pad);
    const uint16_t nav = navigation(input);
    const int count = tabs[tab].count;
    const int item = selected_item();

    if (p & INPUT_GAMEPAD_BACK)
        return MENU_EXIT;
    if (p & INPUT_GAMEPAD_LEFT_SHOULDER)
        tab = (tab + TABS - 1) % TABS;
    if (p & INPUT_GAMEPAD_RIGHT_SHOULDER)
        tab = (tab + 1) % TABS;
    if (p & (INPUT_GAMEPAD_LEFT_SHOULDER | INPUT_GAMEPAD_RIGHT_SHOULDER))
        return MENU_NONE;
    if ((p & INPUT_GAMEPAD_B) && tab != TAB_PLAY) {
        tab = TAB_PLAY;
        return MENU_NONE;
    }
    if ((p & INPUT_GAMEPAD_X) && item == ITEM_APP) {
        open_keyboard(ITEM_APP, pad);
        return MENU_NONE;
    }

    if (nav & INPUT_GAMEPAD_DPAD_UP)
        cursor[tab] = (cursor[tab] + count - 1) % count;
    if (nav & INPUT_GAMEPAD_DPAD_DOWN)
        cursor[tab] = (cursor[tab] + 1) % count;
    if (tab != TAB_PLAY) {
        if (nav & INPUT_GAMEPAD_DPAD_LEFT)
            change(item, -1);
        if (nav & INPUT_GAMEPAD_DPAD_RIGHT)
            change(item, 1);
    }
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START))
        return activate(selected_item(), pad);
    return MENU_NONE;
}

/* ---- drawing ---- */

static bool message_is_error(const char *text)
{
    static const char *const words[] = { "Could not", "could not", "failed", "lost", "No app", "Another app",
                                         "error", "did not" };
    for (int i = 0; i < COUNT(words); i++)
        if (strstr(text, words[i]))
            return true;
    return false;
}

/* What the selected item does, and a warning when its value hurts. */
static void item_help(int item, const char **help, const char **warning)
{
    *warning = NULL;
    switch (item) {
    case ITEM_CONNECT:
        *help = config->host[0] ? "Starts the app on your PC and streams it. The first time, the console pairs with the "
                                  "PC: enter the PIN it shows in Sunshine's web UI."
                                : "Enter the IP address of the PC running Sunshine or Apollo to begin.";
        break;
    case ITEM_HOST:
        *help = "The IP address of the PC running Sunshine or Apollo, e.g. 192.168.1.100. A opens the keyboard.";
        break;
    case ITEM_APP:
        *help = "The app to start on the PC. A picks one from the PC's list; X types a name.";
        break;
    case ITEM_EXIT:
        *help = "Leaves Moonlight. A stream's app keeps running on the PC.";
        break;
    case ITEM_RESOLUTION:
        *help = "The stream's size. 1080p at 30 fps needs the PC to send CAVLC (in Apollo/Sunshine: Prefer CAVLC "
                "over CABAC) and Fast decode on.";
        if (config->height > 900 && config->fps > 30)
            *warning = "1080p at 60 fps decodes at about 50 fps on this console: 1600x900 keeps 60.";
        else if (config->height > 720 && !config->fast_decode)
            *warning = "Turn Fast decode on for 1080p (Video tab): without it frames take about 34 ms, just over 30 fps.";
        break;
    case ITEM_FPS:
        *help = "Frames a second. 60 needs twice the decoding: fine at 720p, too much at 1080p.";
        break;
    case ITEM_BITRATE:
        *help = "Higher looks sharper. Over Ethernet up to 25-30 Mbps works; on the console's Wi-Fi keep 6 to 10.";
        break;
    case ITEM_FAST_DECODE:
        *help = "Skips H.264's deblocking filter: about 30% faster decoding, a slightly blockier picture. Needed for "
                "1080p.";
        break;
    case ITEM_GPU_DECODE:
        *help = "Experimental: the GPU predicts each frame and the CPU only parses it. Exact, but slower than CPU "
                "decoding for now. Turns deblocking off. If anything fails it switches back to the CPU by itself.";
        if (config->gpu_decode)
            *warning = "Beta: expect a lower frame rate than with it off, for now.";
        break;
    case ITEM_STATS:
        *help = "Shows frame rate, decode time and drops at the top of the screen while streaming.";
        break;
    case ITEM_AUDIO_ON_HOST:
        *help = "Where the game's sound plays. Apollo/Sunshine still send the audio stream either way, so this "
                "doesn't lighten the console's Wi-Fi.";
        break;
    case ITEM_AUDIO_PACKETS:
        *help = "How much sound each packet carries. 20 ms keeps the packet rate low.";
        if (config->audio_packet_ms != 20)
            *warning = "Shorter packets flood the console's Wi-Fi with tiny packets: it stops receiving until you "
                       "reboot.";
        break;
    case ITEM_PACKET_SIZE:
        *help = "Video packet size. 1392 means fewer packets a second, which the console's Wi-Fi handles best.";
        break;
    case ITEM_DECODE:
        *help = "Test switch. Off: video is received but never decoded; the picture stays frozen.";
        if (!config->decode)
            *warning = "Video decoding is off: turn it back on to play.";
        break;
    case ITEM_DRAW:
        *help = "Test switch. Off: video is decoded but never drawn.";
        if (!config->draw)
            *warning = "Drawing is off: turn it back on to play.";
        break;
    default:
        *help = "";
        break;
    }
}

static void draw_tabs(void)
{
    enum { GAP = 40 };
    int32_t total = 0;
    for (int t = 0; t < TABS; t++)
        total += gfx_text_width(tabs[t].name, 2) + (t ? GAP : 0);
    const int32_t cy = THEME_HEADER_Y + 24;
    int32_t x = THEME_RIGHT - 38 - 24 - total;
    gfx_button(x - 24 - 38, cy, 'L');
    gfx_button(THEME_RIGHT - 38, cy, 'R');
    for (int t = 0; t < TABS; t++) {
        const int32_t w = gfx_text_width(tabs[t].name, 2);
        gfx_text(x, cy - 22, 2, t == tab ? THEME_TEXT : THEME_TEXT_3, tabs[t].name);
        if (t == tab)
            gfx_rect((float)x, (float)cy + 22.0f, (float)w, 4.0f, 2.0f, THEME_ACCENT);
        x += w + GAP;
    }
}

/* A list row: label on the left, value drawn by the caller on the right. */
static void draw_row(float x, float y, float w, float h, bool selected, bool last)
{
    if (selected)
        gfx_rect(x, y, w, h, 12.0f, THEME_ROW_SELECTED);
    else if (!last)
        gfx_rect(x + 16.0f, y + h, w - 32.0f, 1.0f, 0, THEME_DIVIDER);
}

static void draw_switch(int32_t right, int32_t cy, bool on)
{
    const float w = 60.0f, h = 32.0f, x = (float)right - w, y = (float)cy - h / 2;
    gfx_rect(x, y, w, h, h / 2, on ? THEME_ACCENT : THEME_TRACK);
    gfx_circle(on ? x + w - h / 2 : x + h / 2, (float)cy, h / 2 - 4.0f, on ? THEME_TEXT : THEME_KNOB_OFF);
}

/* A value between arrows (shown on the selected row), right-aligned; returns its left edge. */
static int32_t draw_choice(int32_t right, int32_t y, const char *value, bool selected, uint32_t argb)
{
    const int32_t arrow = gfx_text_width(ARROW_RIGHT, 1);
    const uint32_t arrow_argb = selected ? THEME_ACCENT_TEXT : 0x00000000;
    gfx_text(right - arrow, y + 10, 1, arrow_argb, ARROW_RIGHT);
    const int32_t value_right = right - arrow - 14;
    gfx_text_right(value_right, y, 2, argb, value);
    const int32_t left = value_right - gfx_text_width(value, 2) - 14 - arrow;
    gfx_text(left, y + 10, 1, arrow_argb, ARROW_LEFT);
    return left;
}

static void draw_value(int item, int32_t right, int32_t y, int32_t cy, bool selected)
{
    char value[64];
    const uint32_t text = selected ? THEME_TEXT : THEME_TEXT_2;
    switch (item) {
    case ITEM_RESOLUTION: {
        int r = resolution_index();
        snprintf(value, sizeof value, "%s", r >= 0 ? resolutions[r].name : "custom");
        draw_choice(right, y, value, selected, config->height > 900 && config->fps > 30 ? THEME_WARN : text);
        break;
    }
    case ITEM_FPS:
        snprintf(value, sizeof value, "%d fps", config->fps);
        draw_choice(right, y, value, selected, text);
        break;
    case ITEM_BITRATE: {
        snprintf(value, sizeof value, "%d Mbps", config->bitrate_kbps / 1000);
        const int32_t left = draw_choice(right, y, value, selected, text);
        const float track_w = 200.0f, track_x = (float)left - 28.0f - track_w;
        const float share = (float)(config->bitrate_kbps - MIN_BITRATE_KBPS) / (MAX_BITRATE_KBPS - MIN_BITRATE_KBPS);
        gfx_rect(track_x, (float)cy - 3.0f, track_w, 6.0f, 3.0f, THEME_TRACK);
        gfx_rect(track_x, (float)cy - 3.0f, 6.0f + share * (track_w - 6.0f), 6.0f, 3.0f,
                 selected ? THEME_ACCENT : THEME_ACCENT_SOFT);
        if (selected)
            gfx_circle(track_x + 3.0f + share * (track_w - 6.0f), (float)cy, 9.0f, THEME_TEXT);
        break;
    }
    case ITEM_FAST_DECODE:
        draw_switch(right, cy, config->fast_decode);
        break;
    case ITEM_GPU_DECODE:
        draw_switch(right, cy, config->gpu_decode);
        break;
    case ITEM_STATS:
        draw_switch(right, cy, config->show_stats);
        break;
    case ITEM_DECODE:
        draw_switch(right, cy, config->decode);
        break;
    case ITEM_DRAW:
        draw_switch(right, cy, config->draw);
        break;
    case ITEM_AUDIO_ON_HOST:
        draw_choice(right, y, config->audio_on_host ? "PC" : "Xbox", selected, text);
        break;
    case ITEM_AUDIO_PACKETS:
        snprintf(value, sizeof value, "%d ms", config->audio_packet_ms);
        draw_choice(right, y, value, selected, config->audio_packet_ms != 20 ? THEME_WARN : text);
        break;
    case ITEM_PACKET_SIZE:
        snprintf(value, sizeof value, "%d bytes", config->packet_size);
        draw_choice(right, y, value, selected, text);
        break;
    default:
        break;
    }
}

static const char *item_label(int item)
{
    switch (item) {
    case ITEM_RESOLUTION: return "Resolution";
    case ITEM_FPS: return "Frame rate";
    case ITEM_BITRATE: return "Bitrate";
    case ITEM_FAST_DECODE: return "Fast decode";
    case ITEM_GPU_DECODE: return "GPU decoding (beta)";
    case ITEM_STATS: return "Stats overlay";
    case ITEM_AUDIO_ON_HOST: return "Sound plays on";
    case ITEM_AUDIO_PACKETS: return "Audio packets";
    case ITEM_PACKET_SIZE: return "Video packet size";
    case ITEM_DECODE: return "Decode video";
    case ITEM_DRAW: return "Draw video";
    case ITEM_APP: return "App";
    case ITEM_HOST: return "Host PC";
    default: return "";
    }
}

/* The selected item's help, and its warning, from y down. */
static void draw_help(int32_t y)
{
    const char *help, *warning;
    item_help(selected_item(), &help, &warning);
    char wrapped[512];
    int lines = gfx_wrap(wrapped, sizeof wrapped, help, HELP_CHARS);
    gfx_text(CONTENT_X + 16, y, 1, THEME_TEXT_3, wrapped);
    y += lines * 20 + 14;
    if (warning) {
        gfx_wrap(wrapped, sizeof wrapped, warning, HELP_CHARS - 3);
        gfx_circle(CONTENT_X + 21, (float)y + 10.0f, 5.0f, THEME_WARN);
        gfx_text(CONTENT_X + 36, y, 1, THEME_WARN, wrapped);
    }
}

static void draw_settings_tab(void)
{
    int32_t y = CONTENT_Y;
    const int count = tabs[tab].count;
    for (int i = 0; i < count; i++, y += ROW_H) {
        const int item = tabs[tab].items[i];
        const bool on = i == cursor[tab];
        draw_row(CONTENT_X, (float)y, CONTENT_RIGHT - CONTENT_X, ROW_H, on, i == count - 1 || i + 1 == cursor[tab]);
        const int32_t text_y = y + (ROW_H - 40) / 2;
        gfx_text(CONTENT_X + 24, text_y, 2, on ? THEME_TEXT : THEME_TEXT_2, item_label(item));
        draw_value(item, CONTENT_RIGHT - 24, text_y, y + ROW_H / 2, on);
    }
    draw_help(y + 28);
    if (tab == TAB_DEBUG)
        gfx_text(CONTENT_X + 16, THEME_PANEL_BOTTOM - 20, 1, THEME_TEXT_3,
                 "Moonlight 360, build " __DATE__ " " __TIME__ ". The log is moonlight360.log beside the .xex.");
}

/* The Play tab's picture: a flat moon, a few craters and stars. */
static void draw_moon(float cx, float cy, float r)
{
    gfx_circle(cx, cy, r, 0xFFC9D4F2);
    gfx_circle(cx - r * 0.35f, cy - r * 0.25f, r * 0.20f, 0xFFB6C3E8);
    gfx_circle(cx + r * 0.30f, cy + r * 0.10f, r * 0.13f, 0xFFB6C3E8);
    gfx_circle(cx - r * 0.05f, cy + r * 0.45f, r * 0.10f, 0xFFB6C3E8);
    static const float stars[][3] = { { -1.55f, -0.95f, 2.5f }, { 1.45f, -1.15f, 2.0f }, { 1.70f, 0.70f, 2.5f },
                                      { -1.30f, 1.10f, 2.0f } };
    for (int i = 0; i < COUNT(stars); i++)
        gfx_circle(cx + stars[i][0] * r, cy + stars[i][1] * r, stars[i][2], 0x80C9D4F2);
}

static void draw_play_tab(void)
{
    const int item = selected_item();
    char line[160];
    const int32_t x = CONTENT_X + 16;
    int32_t y = CONTENT_Y + 6;

    draw_moon(960.0f, 300.0f, 100.0f);

    if (config->host[0]) {
        gfx_text(x, y, 1, THEME_ACCENT_TEXT, "READY TO STREAM");
        y += 30;
        gfx_fit(line, sizeof line, config->app, 20);
        gfx_text(x, y, 3, THEME_TEXT, line);
        y += 72;
        const int r = resolution_index();
        snprintf(line, sizeof line, "%s  \xC2\xB7  %s  \xC2\xB7  %d fps  \xC2\xB7  %d Mbps", config->host,
                 r >= 0 ? resolutions[r].name : "custom", config->fps, config->bitrate_kbps / 1000);
        gfx_text(x, y, 1, THEME_TEXT_2, line);
    } else {
        gfx_text(x, y, 1, THEME_ACCENT_TEXT, "WELCOME");
        y += 30;
        gfx_text(x, y, 3, THEME_TEXT, "Set up your PC");
        y += 72;
        gfx_text(x, y, 1, THEME_TEXT_2, "Moonlight streams games from a PC running Sunshine or Apollo.");
    }
    y += 48;

    /* The main button. */
    const float bx = (float)x, by = (float)y, bw = 440.0f, bh = 60.0f;
    const bool on = item == ITEM_CONNECT;
    if (on) {
        gfx_rect(bx, by, bw, bh, bh / 2, THEME_ACCENT);
    } else {
        gfx_rect(bx, by, bw, bh, bh / 2, THEME_BORDER);
        gfx_rect(bx + 1.0f, by + 1.0f, bw - 2.0f, bh - 2.0f, bh / 2 - 1.0f, THEME_BG);
    }
    const char *label = config->host[0] ? ARROW_RIGHT "  Start streaming" : "Set the host PC";
    gfx_text_center((int32_t)(bx + bw / 2), (int32_t)by + 10, 2, on ? THEME_TEXT : THEME_TEXT_2, label);
    y += (int32_t)bh + 36;

    /* App and host. */
    static const int rows[] = { ITEM_APP, ITEM_HOST };
    const float rw = 600.0f;
    for (int i = 0; i < 2; i++, y += ROW_H) {
        const bool sel = item == rows[i];
        draw_row(CONTENT_X, (float)y, rw, ROW_H, sel, i == 1 || item == rows[1]);
        const int32_t ty = y + (ROW_H - 40) / 2;
        gfx_text(x + 8, ty, 2, sel ? THEME_TEXT : THEME_TEXT_2, item_label(rows[i]));
        const char *value = rows[i] == ITEM_APP ? config->app : config->host[0] ? config->host : "Not set";
        gfx_fit(line, sizeof line, value, 16);
        const int32_t right = CONTENT_X + (int32_t)rw - 24;
        gfx_text(right - gfx_text_width("\xE2\x80\xBA", 2), ty, 2, sel ? THEME_ACCENT_TEXT : THEME_TEXT_3,
                 "\xE2\x80\xBA");   /* U+203A */
        gfx_text_right(right - 30, ty, 2,
                       rows[i] == ITEM_HOST && !config->host[0] ? THEME_WARN : sel ? THEME_TEXT : THEME_TEXT_2, line);
    }

    /* The last session's outcome. */
    if (message[0]) {
        const bool error = message_is_error(message);
        char wrapped[640];
        int lines = gfx_wrap(wrapped, sizeof wrapped, message, HELP_CHARS - 6);
        if (lines > 3)
            lines = 3;
        const float my = THEME_PANEL_BOTTOM - (float)(lines * 20) - 26.0f, mh = (float)(lines * 20 + 24);
        gfx_rect(CONTENT_X, my, CONTENT_RIGHT - CONTENT_X, mh, 12.0f, error ? 0x24FF6B6B : 0x204A7BFF);
        gfx_circle(CONTENT_X + 26, my + 22.0f, 6.0f, error ? THEME_ERROR : THEME_ACCENT);
        gfx_text(CONTENT_X + 46, (int32_t)my + 12, 1, error ? 0xFFFFC2C2 : THEME_TEXT_2, wrapped);
    } else {
        gfx_text(x, THEME_PANEL_BOTTOM - 20, 1, THEME_TEXT_3,
                 "While streaming, hold BACK + START for a second to open the stream menu.");
    }
}

static void draw_picker(void)
{
    char line[128];
    const int32_t x = CONTENT_X + 16;
    gfx_text(x, CONTENT_Y, 2, THEME_TEXT, "Choose an app");
    snprintf(line, sizeof line, "on %s", config->host);
    gfx_text(x + gfx_text_width("Choose an app", 2) + 20, CONTENT_Y + 14, 1, THEME_TEXT_3, line);
    AppsState st = apps_state();
    int n = apps_count();
    const float cx = (CONTENT_X + CONTENT_RIGHT) / 2.0f;
    if (st == APPS_LOADING || st == APPS_IDLE) {
        gfx_spinner(cx, 340.0f, 22.0f, THEME_ACCENT_TEXT);
        gfx_text_center((int32_t)cx, 390, 1, THEME_TEXT_2, "Getting the app list from the PC\xE2\x80\xA6");
        return;
    }
    if (st == APPS_FAILED) {
        char text[256], wrapped[300];
        apps_message(text, sizeof text);
        gfx_wrap(wrapped, sizeof wrapped, text, HELP_CHARS - 6);
        gfx_rect(CONTENT_X, PICKER_Y, CONTENT_RIGHT - CONTENT_X, 64.0f, 12.0f, 0x24FF6B6B);
        gfx_circle(CONTENT_X + 26, PICKER_Y + 32.0f, 6.0f, THEME_ERROR);
        gfx_text(CONTENT_X + 46, PICKER_Y + 12, 1, 0xFFFFC2C2, wrapped);
        return;
    }
    snprintf(line, sizeof line, "%d / %d", picker_selected + 1, n);
    gfx_text_right(CONTENT_RIGHT - 16, CONTENT_Y + 14, 1, THEME_TEXT_3, line);
    const int last = (n < picker_top + PICKER_VISIBLE ? n : picker_top + PICKER_VISIBLE) - 1;
    for (int i = picker_top; i <= last; i++) {
        const float y = PICKER_Y + (float)(i - picker_top) * ROW_H;
        char name[64];
        bool running;
        apps_get(i, name, sizeof name, &running);
        const bool on = i == picker_selected;
        draw_row(CONTENT_X, y, CONTENT_RIGHT - CONTENT_X, ROW_H, on, i == last || i + 1 == picker_selected);
        gfx_rect(CONTENT_X + 20, y + 13.0f, 36.0f, 36.0f, 10.0f, on ? THEME_ACCENT : THEME_ACCENT_SOFT);
        const char initial[2] = { name[0] ? name[0] : '?', 0 };
        gfx_text_center(CONTENT_X + 38, (int32_t)y + 21, 1, THEME_TEXT, initial);
        gfx_fit(line, sizeof line, name, 40);
        gfx_text(CONTENT_X + 76, (int32_t)y + (ROW_H - 40) / 2, 2, on ? THEME_TEXT : THEME_TEXT_2, line);
        if (running) {
            const int32_t w = gfx_text_width("Running", 1);
            gfx_circle((float)(CONTENT_RIGHT - 24 - w - 14), y + ROW_H / 2.0f, 5.0f, THEME_GOOD);
            gfx_text_right(CONTENT_RIGHT - 24, (int32_t)y + (ROW_H - 20) / 2, 1, THEME_GOOD, "Running");
        }
    }
    if (picker_top + PICKER_VISIBLE < n)
        gfx_text_center((int32_t)cx, PICKER_Y + PICKER_VISIBLE * ROW_H + 6, 1, THEME_TEXT_3,
                        "\xE2\x96\xBC");   /* U+25BC */
}

void menu_draw(uint32_t width, uint32_t height)
{
    ui_draw_background(width, height);
    ui_draw_header();
    PerfReadings perf;
    perf_get(&perf);
    ui_draw_temperatures(&perf);
    if (picker_open) {
        draw_picker();
        static const char *const labels[] = { "Choose", "Back", "Type a name", "Refresh" };
        ui_draw_hints("ABXY", labels);
        return;
    }
    draw_tabs();
    if (tab == TAB_PLAY)
        draw_play_tab();
    else
        draw_settings_tab();
    const int item = selected_item();
    if (tab == TAB_PLAY && item == ITEM_APP) {
        static const char *const labels[] = { "Choose", "Type a name", NULL, "Settings", "Exit" };
        ui_draw_hints("AXLRK", labels);
    } else if (tab == TAB_PLAY) {
        static const char *const labels[] = { "Select", NULL, "Settings", "Exit" };
        ui_draw_hints("ALRK", labels);
    } else if (is_toggle(item)) {
        static const char *const labels[] = { "Toggle", "Back", NULL, "Tabs" };
        ui_draw_hints("ABLR", labels);
    } else {
        static const char *const labels[] = { "Change", "Back", NULL, "Tabs" };
        ui_draw_hints("DBLR", labels);
    }
}
