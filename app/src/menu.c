#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "about_images.h"
#include "apps.h"
#include "coverart.h"
#include "discover.h"
#include "gfx.h"
#include "icons.h"
#include "logbuf.h"
#include "moon.h"
#include "perf.h"
#include "theme.h"
#include "ui.h"
#include "uisound.h"
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
    ITEM_LOW_LATENCY,
    ITEM_STATS,
    ITEM_AUDIO_ON_HOST,
    ITEM_AUDIO_PACKETS,
    ITEM_PACKET_SIZE,
    ITEM_SLICES,
    ITEM_CODEC,
    ITEM_DECODE,
    ITEM_DRAW,
    ITEM_LOGGING,
    ITEM_SOUNDS,
    ITEM_THEME,
    ITEM_AUTO_RESUME,
    ITEM_HUD_CORNER,
    ITEM_HUD_SIZE,
    ITEM_RUMBLE,
    ITEM_WAKE,
    ITEM_VIDEO_TEST,
    ITEM_SHOW_RECENT,
    ITEM_MENU_COMBO,
    ITEM_LOOK,     /* Fresh or Classic (config.h LOOK_*) */
    ITEM_PLAY_PICTURE,   /* Fresh's Play: the app's cover or the moon */
    ITEM_FORGET,         /* Fresh's PC: forget the PC (A held; menu_fresh.inc) */
    ITEMS,
};

enum {
    TAB_PLAY,
    TAB_VIDEO,
    TAB_NETWORK,
    TAB_OTHER,
    TAB_ABOUT,   /* no rows: what the app is and its credits */
    TABS,
    TAB_ITEMS_MAX = 10,
    TABS_MAX = 8,   /* the most tabs a look has: Fresh's Play, five Settings sections and About */
};

enum {
    KEYBOARD_CHARS = 64,
    MIN_BITRATE_KBPS = 1000,
    MAX_BITRATE_KBPS = 70000,   /* past 50 untried on the console: the Bitrate row warns */
    BITRATE_STEP_KBPS = 1000,
    /* The most worth setting on the console's Wi-Fi: the user's link carried 22 to 25 Mbit/s, and at 30 and 40 set
     * it lost frames (shown 49 and 34 fps; logs, 2026-11-24). More can still be set; the row warns. */
    WIFI_MAX_BITRATE_KBPS = 25000,
    /* Held D-pad or stick: repeats after REPEAT_DELAY frames, then every REPEAT_RATE. */
    REPEAT_DELAY = 24,
    REPEAT_RATE = 5,
    STICK_THRESHOLD = 16000,
    /* Layout. */
    CONTENT_X = 80,
    CONTENT_RIGHT = 1200,
    CONTENT_Y = 136,
    ROW_H = 58,
    SET_ROW_H = 54,   /* a settings row: seven fit on the Video tab with the selected one opened */
    LABEL_X = CONTENT_X + 62,   /* past the row's icon */
    TIP_CHARS = (CONTENT_RIGHT - 24 - LABEL_X) / 10,
    HELP_CHARS = (CONTENT_RIGHT - CONTENT_X - 32) / 10,
    /* The app picker: a row of covers, PICKER_VISIBLE across, the selected one raised and outlined. */
    PICKER_VISIBLE = 5,
    PICKER_Y = 206,
    COVER_W = 180,
    COVER_H = 240,
    COVER_GAP = 28,
    COVER_RADIUS = 16,   /* covers' corners, in the cover's own 192x256 pixels (about 15 on screen in the picker) */
};

typedef struct {
    const char *name;
    int count;
    int items[TAB_ITEMS_MAX];
} Tab;

/* Classic: five tabs along the top. */
static const Tab classic_tabs[TABS] = {
    { "Play", 3, { ITEM_CONNECT, ITEM_APP, ITEM_HOST } },
    { "Video", 9, { ITEM_RESOLUTION, ITEM_FPS, ITEM_BITRATE, ITEM_CODEC, ITEM_LOW_LATENCY, ITEM_FAST_DECODE,
                   ITEM_STATS, ITEM_HUD_CORNER, ITEM_HUD_SIZE } },
    { "Network", 4, { ITEM_WAKE, ITEM_AUDIO_PACKETS, ITEM_PACKET_SIZE, ITEM_SLICES } },
    { "Other", 10, { ITEM_AUTO_RESUME, ITEM_SHOW_RECENT, ITEM_MENU_COMBO, ITEM_AUDIO_ON_HOST, ITEM_SOUNDS, ITEM_RUMBLE,
                     ITEM_LOOK, ITEM_THEME, ITEM_VIDEO_TEST, ITEM_LOGGING } },
    { "About", 0, { 0 } },
};

/* Fresh: Play, Settings (its sections, chosen from a list at the left) and About (menu_fresh.inc). */
enum { FRESH_TABS = 8, FRESH_ABOUT = 7 };
static const Tab fresh_tabs[FRESH_TABS] = {
    { "Play", 1, { ITEM_CONNECT } },
    { "Quality", 6, { ITEM_RESOLUTION, ITEM_FPS, ITEM_BITRATE, ITEM_CODEC, ITEM_LOW_LATENCY, ITEM_FAST_DECODE } },
    { "PC", 4, { ITEM_HOST, ITEM_WAKE, ITEM_AUTO_RESUME, ITEM_FORGET } },
    { "Controls", 2, { ITEM_MENU_COMBO, ITEM_RUMBLE } },
    { "Sound", 3, { ITEM_AUDIO_ON_HOST, ITEM_SOUNDS, ITEM_AUDIO_PACKETS } },
    { "Look", 7, { ITEM_LOOK, ITEM_PLAY_PICTURE, ITEM_THEME, ITEM_SHOW_RECENT, ITEM_STATS, ITEM_HUD_CORNER,
                   ITEM_HUD_SIZE } },
    { "Advanced", 4, { ITEM_PACKET_SIZE, ITEM_SLICES, ITEM_VIDEO_TEST, ITEM_LOGGING } },
    { "About", 0, { 0 } },
};

/* The look's tabs (set_look). */
static const Tab *tabs = classic_tabs;
static int tab_count = TABS;

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
/* Not 40: the host (Vibeshine, 2026-11-24) gave no working stream when asked for it. */
static const int audio_durations[] = { 5, 10, 20 };   /* shortest first; 20 is the default */
/* No 512/256: smaller packets mean more packets per second, and a flood of small packets (5 ms audio, or 256-byte
 * video at ~4000/s) makes the console's Wi-Fi stop receiving until reboot (2026-11-22). */
static const int packet_sizes[] = { 1392, 1024 };
static const int slice_counts[] = { 6, 12, 24, 48 };

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define ARROW_LEFT "\xE2\x97\x80"    /* U+25C0 */
#define ARROW_RIGHT "\xE2\x96\xB6"   /* U+25B6 */

static Config *config;
static const char *config_path;
static int tab;
static int cursor[TABS_MAX];
static char message[512];
/* Settings: the selected row also shows what it does (Y turns it on and off). Warnings show either way. */
static bool show_details;
/* The About tab shows the log instead of the credits (Y, while logging is on). */
static bool about_log;

/* The on-screen keyboard, while it's open. */
static XAM_OVERLAPPED keyboard_overlapped;
static uint16_t keyboard_text[KEYBOARD_CHARS + 1];
static uint16_t keyboard_title[48], keyboard_description[96], keyboard_default[KEYBOARD_CHARS + 1];
static int keyboard_item = -1;

/* The app picker, while it's open. */
static bool picker_open;
static int picker_selected, picker_top;
static bool picker_synced;   /* the selection was put on the App setting's app once the list came */
/* The Play tab's Recent row: whether the cursor is on it, and which cover. */
static bool recent_focus;
static int recent_index;

static bool message_is_error(const char *text);

/* The Recent row's apps; none while there's no PC, or an error to show (its box takes the row's place). */
static int recent_count(void)
{
    int n = 0;
    while (n < CONFIG_RECENT && config->recent_name[n][0])
        n++;
    return config->show_recent && config->host[0] && !(message[0] && message_is_error(message)) ? n : 0;
}

/* The covers' textures, made from apps.c's pixels as they come; dropped when the list is fetched again. */
static GfxImage *cover_images[APPS_MAX];
static unsigned cover_generation;

/* D-pad and stick repeat. */
static uint16_t nav_held;
static uint32_t nav_next;

static int selected_item(void)
{
    return tabs[tab].items[cursor[tab]];
}

static void select_item(int item)
{
    for (int t = 0; t < tab_count; t++)
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

static bool fresh(void)
{
    return config->look != LOOK_CLASSIC;
}

/* The tabs of the look in use; ui.c and overlay.c follow theme_look. */
static void set_look(void)
{
    tabs = fresh() ? fresh_tabs : classic_tabs;
    tab_count = fresh() ? FRESH_TABS : TABS;
    theme_look = config->look;
    if (tab >= tab_count)
        tab = TAB_PLAY;
}

void menu_open(Config *c, const char *path)
{
    config = c;
    config_path = path;
    theme_set(config->theme);
    set_look();
    select_item(config->host[0] || fresh() ? ITEM_CONNECT : ITEM_HOST);   /* Fresh's Play welcomes a new user */
}

static bool on_wifi;

void menu_set_wifi(bool wifi)
{
    on_wifi = wifi;
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
    return item == ITEM_FAST_DECODE || item == ITEM_LOW_LATENCY || item == ITEM_DECODE ||
           item == ITEM_DRAW || item == ITEM_LOGGING || item == ITEM_SOUNDS || item == ITEM_AUTO_RESUME ||
           item == ITEM_RUMBLE || item == ITEM_WAKE || item == ITEM_SHOW_RECENT;
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
        config->stats_mode = step_choice(config->stats_mode, STATS_MODES, step);
        break;
    case ITEM_FAST_DECODE:
        config->fast_decode = !config->fast_decode;
        break;
    case ITEM_LOW_LATENCY:
        config->low_latency = !config->low_latency;
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
    case ITEM_SLICES:
        config->slices = slice_counts[step_choice(
            index_of(slice_counts, COUNT(slice_counts), config->slices), COUNT(slice_counts), step)];
        break;
    case ITEM_CODEC:
        config->codec = config->codec == CODEC_MPEG2 ? CODEC_H264 : CODEC_MPEG2;
        break;
    case ITEM_DECODE:
        config->decode = !config->decode;
        break;
    case ITEM_DRAW:
        config->draw = !config->draw;
        break;
    case ITEM_LOGGING:
        config->logging = !config->logging;
        break;
    case ITEM_SOUNDS:
        config->ui_sounds = !config->ui_sounds;
        break;
    case ITEM_AUTO_RESUME:
        config->auto_resume = !config->auto_resume;
        break;
    case ITEM_RUMBLE:
        config->rumble = !config->rumble;
        break;
    case ITEM_SHOW_RECENT:
        config->show_recent = !config->show_recent;
        break;
    case ITEM_MENU_COMBO:
        config->menu_combo = step_choice(config->menu_combo, MENU_COMBOS, step);
        break;
    case ITEM_WAKE:
        config->wake_on_lan = !config->wake_on_lan;
        break;
    case ITEM_HUD_CORNER:
        config->hud_corner = step_choice(config->hud_corner, HUD_CORNERS, step);
        break;
    case ITEM_HUD_SIZE:
        config->hud_size = step_choice(config->hud_size, HUD_SIZES, step);
        break;
    case ITEM_VIDEO_TEST: {   /* full, no drawing, no decoding: the two test switches as one */
        const int now = !config->decode ? 2 : !config->draw ? 1 : 0, next = step_choice(now, 3, step);
        config->decode = next != 2;
        config->draw = next == 0;
        break;
    }
    case ITEM_THEME:
        config->theme = step_choice(config->theme, THEMES, step);
        theme_set(config->theme);
        break;
    case ITEM_PLAY_PICTURE:
        config->play_moon = !config->play_moon;
        break;
    case ITEM_LOOK:   /* the other look's tabs, on the same row */
        config->look = config->look == LOOK_CLASSIC ? LOOK_FRESH : LOOK_CLASSIC;
        set_look();
        select_item(ITEM_LOOK);
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
            config->app_id = 0;   /* a typed name: its id (and cover) come once it streams */
            select_item(ITEM_CONNECT);
        }
        save();
    }
    keyboard_item = -1;
    return false;
}

/* ---- Fresh (menu_fresh.inc, included at the end) ---- */

static MenuAction fresh_update_play(uint16_t p, uint16_t nav, uint32_t pad);
static void fresh_draw(uint32_t width, uint32_t height);
static bool fresh_startup_skip(const InputState *input);
static bool fresh_update_nav(uint16_t p, uint16_t nav, uint32_t pad, MenuAction *action);

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
    picker_synced = false;
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
        uisound_play(UISOUND_BACK);
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
    if (!picker_synced) {   /* start on the app already chosen */
        picker_synced = true;
        for (int i = 0; i < n; i++) {
            char name[64];
            bool running;
            apps_get(i, name, sizeof name, &running);
            if (!strcmp(name, config->app))
                picker_selected = i;
        }
    }
    /* Along the row: left and right (up and down too), stopping at its ends. */
    const int before = picker_selected;
    if ((nav & (INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_UP)) && picker_selected > 0)
        picker_selected--;
    if ((nav & (INPUT_GAMEPAD_DPAD_RIGHT | INPUT_GAMEPAD_DPAD_DOWN)) && picker_selected < n - 1)
        picker_selected++;
    if (picker_selected != before)
        uisound_play(UISOUND_MOVE);
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
        uisound_play(UISOUND_SELECT);
        bool running;
        char name[64];
        apps_get(picker_selected, name, sizeof name, &running);
        snprintf(config->app, sizeof config->app, "%s", name);
        config->app_id = apps_id(picker_selected);
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

/* ---- finding the PC: the PCs on the network, and typing an address ---- */

static bool finder_open;
static int finder_selected;

static void open_finder(void)
{
    finder_open = true;
    finder_selected = 0;
    discover_start();
}

static void update_finder(const InputState *input)
{
    uint32_t pad;
    const uint16_t p = pressed_buttons(input, &pad);
    const uint16_t nav = navigation(input);
    const int rows = discover_count() + 1;   /* the PCs found, then "Type an address" */
    if (p & INPUT_GAMEPAD_B) {
        finder_open = false;
        uisound_play(UISOUND_BACK);
        return;
    }
    if ((p & INPUT_GAMEPAD_Y) && !discover_running()) {
        uisound_play(UISOUND_CHANGE);
        discover_start();
        finder_selected = 0;
        return;
    }
    const int before = finder_selected;
    if ((nav & INPUT_GAMEPAD_DPAD_UP) && finder_selected > 0)
        finder_selected--;
    if ((nav & INPUT_GAMEPAD_DPAD_DOWN) && finder_selected < rows - 1)
        finder_selected++;
    if (finder_selected > rows - 1)
        finder_selected = rows - 1;
    if (finder_selected != before)
        uisound_play(UISOUND_MOVE);
    const bool type = (p & INPUT_GAMEPAD_X) || ((p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) && finder_selected == rows - 1);
    if (type) {
        uisound_play(UISOUND_SELECT);
        finder_open = false;
        discover_stop();
        open_keyboard(ITEM_HOST, pad);
        return;
    }
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
        uisound_play(UISOUND_SELECT);
        char address[16], name[64];
        discover_get(finder_selected, address, sizeof address, name, sizeof name);
        snprintf(config->host, sizeof config->host, "%s", address);
        save();
        discover_stop();
        finder_open = false;
        menu_set_message(NULL);
        select_item(ITEM_CONNECT);
    }
}

/* ---- the menu ---- */

static MenuAction activate(int item, uint32_t pad)
{
    switch (item) {
    case ITEM_CONNECT:
        if (!config->host[0]) {
            open_finder();
            return MENU_NONE;
        }
        return MENU_CONNECT;
    case ITEM_HOST:
        open_finder();
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

/* This frame's input, for what goes by a button held (Fresh's Forget this PC). */
static const InputState *update_input;

static MenuAction update(const InputState *input)
{
    update_input = input;
    if (poll_keyboard())
        return MENU_NONE;
    if (fresh_startup_skip(input))   /* the startup takes the buttons: any one skips it */
        return MENU_NONE;
    if (picker_open) {
        update_picker(input);
        return MENU_NONE;
    }
    if (finder_open) {
        update_finder(input);
        return MENU_NONE;
    }
    uint32_t pad;
    const uint16_t p = pressed_buttons(input, &pad);
    const uint16_t nav = navigation(input);
    const int count = tabs[tab].count;
    const int item = selected_item();

    if (p & INPUT_GAMEPAD_BACK)
        return MENU_EXIT;
    MenuAction fresh_action;
    if (fresh() && fresh_update_nav(p, nav, pad, &fresh_action))   /* Fresh's places and Settings' sections */
        return fresh_action;
    if (p & INPUT_GAMEPAD_LEFT_SHOULDER)
        tab = (tab + tab_count - 1) % tab_count;
    if (p & INPUT_GAMEPAD_RIGHT_SHOULDER)
        tab = (tab + 1) % tab_count;
    if (p & (INPUT_GAMEPAD_LEFT_SHOULDER | INPUT_GAMEPAD_RIGHT_SHOULDER)) {
        uisound_play(UISOUND_MOVE);
        return MENU_NONE;
    }
    if ((p & INPUT_GAMEPAD_B) && tab != TAB_PLAY) {
        tab = TAB_PLAY;
        uisound_play(UISOUND_BACK);
        return MENU_NONE;
    }
    if (count == 0) {   /* the About tab: nothing to select; Y shows the log while logging is on */
        if ((p & INPUT_GAMEPAD_Y) && config->logging) {
            about_log = !about_log;
            uisound_play(UISOUND_CHANGE);
        }
        return MENU_NONE;
    }
    if (fresh() && tab == TAB_PLAY)
        return fresh_update_play(p, nav, pad);
    if ((p & INPUT_GAMEPAD_Y) && tab != TAB_PLAY && !fresh()) {   /* Fresh shows what a setting does beside it */
        show_details = !show_details;
        uisound_play(UISOUND_CHANGE);
        return MENU_NONE;
    }
    if ((p & INPUT_GAMEPAD_X) && item == ITEM_APP && !recent_focus) {
        open_keyboard(ITEM_APP, pad);
        return MENU_NONE;
    }
    /* The Play tab's Recent row, to the right of the rest: right (or down from Host PC) onto it, at the cover it was
     * left on; left past its first cover, up, or B back to the row it came from. A streams the app straight away,
     * Y only chooses it, X takes it off the list. */
    if (tab == TAB_PLAY) {
        static int came_from = ITEM_CONNECT;
        const int rc = recent_count();
        if (recent_focus && rc == 0)
            recent_focus = false;
        if (recent_focus) {
            if (recent_index >= rc)
                recent_index = rc - 1;
            const bool leave = (p & INPUT_GAMEPAD_B) || (nav & INPUT_GAMEPAD_DPAD_UP) ||
                               ((nav & INPUT_GAMEPAD_DPAD_LEFT) && recent_index == 0);
            if (leave) {
                recent_focus = false;
                select_item(came_from);
                uisound_play((p & INPUT_GAMEPAD_B) ? UISOUND_BACK : UISOUND_MOVE);
                return MENU_NONE;
            }
            if (nav & INPUT_GAMEPAD_DPAD_DOWN) {   /* down, round to the main button */
                recent_focus = false;
                select_item(ITEM_CONNECT);
                uisound_play(UISOUND_MOVE);
                return MENU_NONE;
            }
            const int before = recent_index;
            if ((nav & INPUT_GAMEPAD_DPAD_LEFT) && recent_index > 0)
                recent_index--;
            if ((nav & INPUT_GAMEPAD_DPAD_RIGHT) && recent_index < rc - 1)
                recent_index++;
            if (recent_index != before)
                uisound_play(UISOUND_MOVE);
            if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START | INPUT_GAMEPAD_Y)) {
                const bool stream = !(p & INPUT_GAMEPAD_Y);
                uisound_play(stream ? UISOUND_SELECT : UISOUND_CHANGE);
                snprintf(config->app, sizeof config->app, "%s", config->recent_name[recent_index]);
                config->app_id = config->recent_id[recent_index];
                save();
                if (!stream)
                    return MENU_NONE;   /* chosen: the dot moves to it, and the cursor stays */
                recent_focus = false;
                select_item(ITEM_CONNECT);
                return MENU_CONNECT;
            }
            if (p & INPUT_GAMEPAD_X) {
                uisound_play(UISOUND_BACK);
                config_remove_recent(config, recent_index);
                save();
                if (recent_count() == 0) {
                    recent_focus = false;
                    select_item(came_from);
                }
            }
            return MENU_NONE;
        }
        if (rc && ((nav & INPUT_GAMEPAD_DPAD_RIGHT) || ((nav & INPUT_GAMEPAD_DPAD_DOWN) && item == ITEM_HOST))) {
            came_from = item;
            recent_focus = true;
            if (recent_index >= rc || recent_index < 0)
                recent_index = 0;
            uisound_play(UISOUND_MOVE);
            return MENU_NONE;
        }
    }

    if ((nav & (INPUT_GAMEPAD_DPAD_UP | INPUT_GAMEPAD_DPAD_DOWN)) && count > 1)
        uisound_play(UISOUND_MOVE);
    if (nav & INPUT_GAMEPAD_DPAD_UP)
        cursor[tab] = (cursor[tab] + count - 1) % count;
    if (nav & INPUT_GAMEPAD_DPAD_DOWN)
        cursor[tab] = (cursor[tab] + 1) % count;
    if (tab != TAB_PLAY && (nav & (INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_RIGHT))) {
        uisound_play(UISOUND_CHANGE);
        change(item, (nav & INPUT_GAMEPAD_DPAD_LEFT) ? -1 : 1);
    }
    if (p & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_START)) {
        uisound_play(is_toggle(selected_item()) ? UISOUND_CHANGE : UISOUND_SELECT);
        return activate(selected_item(), pad);
    }
    return MENU_NONE;
}

/* Start streaming answers the press before the menu gives way to the connection's steps: the main button dips, springs
 * back and glows, light sweeps across it and its arrow shoots off (draw_play_tab), about 0.4 s. */
enum { LAUNCH_FRAMES = 24 };
static bool launching;
static uint32_t launch_frame;

MenuAction menu_update(const InputState *input)
{
    if (launching) {
        if (gfx_frame() - launch_frame < LAUNCH_FRAMES)
            return MENU_NONE;
        launching = false;
        return MENU_CONNECT;
    }
    const MenuAction action = update(input);
    if (action != MENU_CONNECT)
        return action;
    launching = true;
    launch_frame = gfx_frame();
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

/* What a failed connection's message most likely means, and what to do about it, in plain words. The original message
 * is shown under them, as it was. NULL title: no plain version (not an error, or one this doesn't know). */
static void friendly_error(const char *raw, const char **title, const char **advice)
{
    static const struct {
        const char *match, *title, *advice;
    } known[] = {
        { "Could not reach", "Can't reach the PC",
          "Check the PC is on and Sunshine or Apollo is running, and that Host PC is its address. Wake the PC "
          "(Network tab) can wake a sleeping one." },
        { "Pairing failed", "Pairing didn't finish",
          "Enter the PIN on Sunshine's web UI (its PIN page) within a minute of it showing here, then try again." },
        { "No app named", "That app isn't on the PC", "Pick one from the PC's list: App, on this tab." },
        { "Could not get the app list", "The PC didn't send its app list",
          "Sunshine may be starting up or restarting: try again in a moment." },
        { "Another app is running", "Another app is running on the PC",
          "Close it on the PC (or Quit it from the stream menu), then try again." },
        { "Could not start \"", "The PC couldn't start the app",
          "Check the app runs on the PC, and that the PC isn't already streaming to another device." },
        { "Could not connect the stream", "The stream didn't start",
          "A firewall on the PC may block the stream's ports (UDP 47998-48010). Try again; if it keeps failing, "
          "restart Sunshine." },
        { "Connection lost", "The connection dropped",
          "The Wi-Fi may have hiccuped. Try again, or lower the Bitrate (Video tab)." },
        { "Could not create keys", "Couldn't set up the console's keys",
          "Check there's free space on the drive Moonlight is on." },
        { "Could not load the new keys", "Couldn't set up the console's keys",
          "Check there's free space on the drive Moonlight is on." },
        { "Could not start the session thread", "Moonlight couldn't start connecting", "Restart Moonlight." },
        { "closing \"", "The app may still be running on the PC", "Close it on the PC if it's still open." },
    };
    *title = *advice = NULL;
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strstr(raw, known[i].match)) {
            *title = known[i].title;
            *advice = known[i].advice;
            return;
        }
}

/* 1080p at 60 fps: 23 ms a frame as hardware encoders send it, 12 from x264's ultrafast preset, which leaves out the
 * deblocking filter and sub-pixel motion (decbench, 2026-11-23). README: "Lean stream". */
#define LEAN_NOTICE \
    "1080p at 60 fps needs a lean stream: in Sunshine, Apollo or Vibeshine on the PC, force the Software encoder " \
    "and set its preset to ultrafast. The stats then say \"lean\"."

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
        *help = "The PC running Sunshine or Apollo. A searches the network for it, or lets you type its address.";
        break;
    case ITEM_APP:
        *help = "The app to start on the PC. A picks one from the PC's list; X types a name.";
        break;
    case ITEM_EXIT:
        *help = "Leaves Moonlight. A stream's app keeps running on the PC.";
        break;
    case ITEM_RESOLUTION:
        *help = "The stream's size. 1080p needs the PC to send CAVLC (in Sunshine, Apollo or Vibeshine: Prefer CAVLC "
                "over CABAC; the Software encoder's ultrafast preset is CAVLC by itself).";
        if (config->height > 900 && config->fps > 30)
            *warning = LEAN_NOTICE;
        else if (config->height > 900 && !config->fast_decode)
            *warning = "With a hardware encoder's stream, 1080p needs Skip deblocking on (Video tab). A lean stream "
                       "(the PC's Software encoder, ultrafast preset) doesn't.";
        break;
    case ITEM_FPS:
        *help = "Frames a second. 60 needs twice the decoding: fine up to 1600x900; at 1080p it needs a lean stream.";
        if (config->height > 900 && config->fps > 30)
            *warning = LEAN_NOTICE;
        break;
    case ITEM_BITRATE:
        *help = "Higher looks sharper, and takes the console longer to decode. With a network cable 30 Mbps works at "
                "1080p and 60 fps; on the console's Wi-Fi keep to 25 or less.";
        /* A lean 1080p60 stream decodes in 12.8 ms at 30 Mbps (logs, 2026-11-24) and about 1.25 ms more for every
         * 100 kbit a frame (decbench): near 15 ms at 40 Mbps, past the 16.7 there are at 50. Not yet tried. */
        if (on_wifi && config->bitrate_kbps > WIFI_MAX_BITRATE_KBPS)
            *warning = "On Wi-Fi keep to 25 Mbps or less: the console's Wi-Fi carries about that much, and past it "
                       "frames are lost and the picture stutters. A network cable takes more.";
        else if (config->bitrate_kbps > 40000 && config->height > 900 && config->fps > 30)
            *warning = "Past 40 Mbps the console probably can't decode 1080p at 60 fps in time: frames would drop. "
                       "Watch the stats.";
        else if (config->bitrate_kbps > 50000)
            *warning = "Past 50 Mbps is untried on the console: it may not decode every frame in time, and needs a "
                       "network cable. Watch the stats.";
        break;
    case ITEM_FAST_DECODE:
        *help = "About 30% faster decoding: skips H.264's deblocking filter, so the picture is a little blockier. "
                "Needed for 1080p, unless the PC sends a lean stream (then leave it off).";
        break;
    case ITEM_LOW_LATENCY:
        *help = "Draws each frame the moment it's decoded instead of on a fixed beat, so it reaches the TV up to a "
                "refresh sooner. No tearing. Turn it off if the picture stutters.";
        break;
    case ITEM_STATS: {
        static char text[320];
        snprintf(text, sizeof text,
                 "What shows over the game. Compact: frame rate, latency, ping and loss in one line. Detailed: where "
                 "the latency goes, the network and the console's load. Also changes in the stream menu (hold %s).",
                 menu_combo_name(config->menu_combo));
        *help = text;
        break;
    }
    case ITEM_AUDIO_ON_HOST:
        *help = "Where the game's sound plays. Apollo/Sunshine still send the audio stream either way, so this "
                "doesn't lighten the console's Wi-Fi.";
        break;
    case ITEM_AUDIO_PACKETS:
        *help = "How much sound each packet carries: longer packets mean fewer of them. 20 ms suits a cable and "
                "Wi-Fi.";
        if (config->audio_packet_ms < 20)
            *warning = on_wifi ? "On Wi-Fi the stream gets 20 ms packets whatever is set here: shorter ones make the "
                                 "console's Wi-Fi adapter stop receiving until a reboot."
                               : "Only with a network cable: on Wi-Fi the app keeps to 20 ms, as shorter packets make "
                                 "the console's Wi-Fi adapter stop receiving until a reboot.";
        break;
    case ITEM_PACKET_SIZE:
        *help = "Video packet size. 1392 means fewer packets a second, which the console's Wi-Fi handles best.";
        break;
    case ITEM_SLICES:
        *help = "Pieces the PC cuts each frame into. With more than the console's 6 decoding threads, a thread "
                "that finishes an easy piece takes the next one, so busy scenes decode faster. 48 is being tested.";
        break;
    case ITEM_CODEC:
        *help = "MPEG-2 is lighter for the console to decode than H.264, but needs more bitrate for the same picture. "
                "The PC must run Sunshine with mpeg2_mode = 2; any other host sends H.264.";
        if (config->codec == CODEC_MPEG2)
            *warning = "Experimental: MPEG-2 is still being tested and may not work. Choose H.264 if the stream "
                       "fails or looks wrong.";
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
    case ITEM_LOGGING:
        *help = "Writes moonlight360.log beside the .xex, for finding problems.";
        if (config->logging)
            *warning = "Logging is on: the menus may stutter now and then.";
        break;
    case ITEM_SOUNDS:
        *help = "Soft clicks as you move around the menus. They stop while a stream plays.";
        break;
    case ITEM_HUD_CORNER:
        *help = "Which corner of the screen the stats sit in while streaming.";
        break;
    case ITEM_HUD_SIZE:
        *help = "How big the stats are while streaming: smaller covers less of the game.";
        break;
    case ITEM_RUMBLE:
        *help = "Passes the game's rumble on to the controllers.";
        if (on_wifi && config->rumble)
            *warning = "No rumble while the console is on Wi-Fi: passing it on to the controller during a stream "
                       "makes the console's Wi-Fi adapter stop receiving until a reboot. With a network cable it "
                       "works.";
        break;
    case ITEM_SHOW_RECENT:
        *help = "Shows the apps you streamed lately as covers on the Play tab, for one press to stream them again.";
        break;
    case ITEM_MENU_COMBO:
        *help = "What you hold for a second while streaming to open the stream menu. The game also gets the press: "
                "pick one it doesn't use much.";
        break;
    case ITEM_WAKE:
        *help = "Wakes a sleeping PC (Wake-on-LAN) before connecting. Wake-on-LAN must be on in the PC's BIOS and "
                "network adapter. Learns the PC's address the first time it connects.";
        if (config->wake_on_lan && !config->host_mac[0])
            *warning = "Connect once while the PC is awake, so the console learns its address.";
        break;
    case ITEM_VIDEO_TEST:
        *help = "Test modes. No drawing: video is decoded but never shown. No decoding: video is received only.";
        if (!config->decode || !config->draw)
            *warning = "A test mode is on: set it back to Full to play.";
        break;
    case ITEM_AUTO_RESUME:
        *help = "When Moonlight opens, it reconnects to the last app straight away, as Start streaming does.";
        if (config->auto_resume && !config->host[0])
            *warning = "Set the Host PC first (Play tab).";
        break;
    case ITEM_THEME:
        *help = "The menus' accent colour: the selection, buttons, switches and the logo.";
        break;
    case ITEM_FORGET:
        *help = config->host[0]
                    ? "Removes this PC from Moonlight: its address, its address for waking it and the recent apps. "
                      "Hold A for a second. The PC still lists the console as paired (Sunshine's web UI, Clients), "
                      "where it can be removed too."
                    : "No PC is set: Play finds one.";
        break;
    case ITEM_PLAY_PICTURE:
        *help = "What Play shows on the right: the app's cover, lighting the page in its colour, or the moon and its "
                "night sky, with its rare events now and then.";
        break;
    case ITEM_LOOK:
        *help = "How the menus are drawn. Fresh: Play, Settings and About, your covers first. Classic: five tabs and "
                "the night sky. Settings stay the same; it changes right away.";
        break;
    default:
        *help = "";
        break;
    }
}

static float approach(float now, float target);

static void draw_tabs(void)
{
    enum { GAP = 26 };
    int32_t total = 0;
    for (int t = 0; t < tab_count; t++)
        total += gfx_text_width(tabs[t].name, 2) + (t ? GAP : 0);
    const int32_t cy = THEME_HEADER_Y + 24;
    int32_t x = THEME_RIGHT - 38 - 24 - total;
    gfx_button(x - 24 - 38, cy, 'L');
    gfx_button(THEME_RIGHT - 38, cy, 'R');
    static float line_x = -1.0f, line_w;
    for (int t = 0; t < tab_count; t++) {
        const int32_t w = gfx_text_width(tabs[t].name, 2);
        gfx_text(x, cy - 22, 2, t == tab ? THEME_TEXT : THEME_TEXT_3, tabs[t].name);
        if (t == tab) {   /* the underline slides over to the new tab */
            line_x = line_x < 0.0f ? (float)x : approach(line_x, (float)x);
            line_w = line_w <= 0.0f ? (float)w : approach(line_w, (float)w);
        }
        x += w + GAP;
    }
    gfx_rect(line_x, (float)cy + 22.0f, line_w, 4.0f, 2.0f, THEME_ACCENT);
}

/* ---- motion: the selection slides to its row, the tab's underline to its tab, and a new tab fades in ---- */

enum {
    FADE_FRAMES = 9,   /* 150 ms at the menus' 60 frames a second */
    LIST_SETTINGS = 1, LIST_PLAY, LIST_PICKER, LIST_FINDER,   /* which list the selection is in; a new one doesn't slide */
};

/* A step of the way from now to target: about 120 ms to settle at 60 frames a second. */
static float approach(float now, float target)
{
    const float d = target - now;
    return d > -0.5f && d < 0.5f ? target : now + d * 0.33f;
}

static struct {
    int list;          /* LIST_*, 0 for none drawn last frame */
    uint32_t frame;    /* gfx_frame when last drawn */
    float y, h;
} selection;

/* The selected row's backdrop and bar, slid from where it was to (x, y, w, h) in list. */
static void draw_selection(int list, float x, float y, float w, float h)
{
    const uint32_t now = gfx_frame();
    if (selection.list != list || now - selection.frame > 2) {   /* a new list, or not shown a moment ago: no slide */
        selection.y = y;
        selection.h = h;
    } else {
        selection.y = approach(selection.y, y);
        selection.h = approach(selection.h, h);
    }
    selection.list = list;
    selection.frame = now;
    gfx_rect(x, selection.y, w, selection.h, 12.0f, THEME_ROW_SELECTED);
    gfx_rect(x, selection.y + 14.0f, 4.0f, selection.h - 28.0f, 2.0f, THEME_ACCENT);   /* the cursor, at a glance */
}

/* A list row: label on the left, value drawn by the caller on the right; the selected row's backdrop is drawn by
 * draw_selection, before the rows. */
static void draw_row(float x, float y, float w, float h, bool selected, bool last)
{
    if (!selected && !last)
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
    const uint32_t arrow_argb = selected ? THEME_ACCENT : 0x00000000;
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
        /* Past what Wi-Fi carries the value says so itself, selected or not. */
        const bool over_wifi = on_wifi && config->bitrate_kbps > WIFI_MAX_BITRATE_KBPS;
        snprintf(value, sizeof value, over_wifi ? "%d Mbps \xC2\xB7 too high for Wi-Fi" : "%d Mbps",
                 config->bitrate_kbps / 1000);
        const int32_t left = draw_choice(right, y, value, selected, over_wifi ? THEME_WARN : text);
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
    case ITEM_LOW_LATENCY:
        draw_switch(right, cy, config->low_latency);
        break;
    case ITEM_STATS: {
        static const char *const names[STATS_MODES] = { "Off", "Compact", "Detailed" };
        draw_choice(right, y, names[config->stats_mode], selected, text);
        break;
    }
    case ITEM_DECODE:
        draw_switch(right, cy, config->decode);
        break;
    case ITEM_DRAW:
        draw_switch(right, cy, config->draw);
        break;
    case ITEM_LOGGING:
        draw_switch(right, cy, config->logging);
        break;
    case ITEM_SOUNDS:
        draw_switch(right, cy, config->ui_sounds);
        break;
    case ITEM_AUTO_RESUME:
        draw_switch(right, cy, config->auto_resume);
        break;
    case ITEM_RUMBLE:
        draw_switch(right, cy, config->rumble);
        break;
    case ITEM_SHOW_RECENT:
        draw_switch(right, cy, config->show_recent);
        break;
    case ITEM_MENU_COMBO: {
        char label[32];
        snprintf(label, sizeof label, "Hold %s", menu_combo_name(config->menu_combo));
        draw_choice(right, y, label, selected, text);
        break;
    }
    case ITEM_WAKE:
        draw_switch(right, cy, config->wake_on_lan);
        break;
    case ITEM_HUD_CORNER: {
        static const char *const corners[HUD_CORNERS] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        draw_choice(right, y, corners[config->hud_corner], selected, text);
        break;
    }
    case ITEM_HUD_SIZE: {
        static const char *const sizes[HUD_SIZES] = { "Small", "Medium", "Large" };
        draw_choice(right, y, sizes[config->hud_size], selected, text);
        break;
    }
    case ITEM_VIDEO_TEST: {
        static const char *const modes[3] = { "Full", "No drawing", "No decoding" };
        const int now = !config->decode ? 2 : !config->draw ? 1 : 0;
        draw_choice(right, y, modes[now], selected, now ? THEME_WARN : text);
        break;
    }
    case ITEM_THEME: {   /* the name and a dot of its colour; the cards under the row show them all */
        const int32_t left = draw_choice(right, y, theme->name, selected, text);
        if (!selected)   /* selected, the dots under the row show it */
            gfx_circle((float)left - 14.0f, (float)cy, 8.0f, THEME_ACCENT);
        break;
    }
    case ITEM_AUDIO_ON_HOST:
        draw_choice(right, y, config->audio_on_host ? "PC" : "Xbox", selected, text);
        break;
    case ITEM_LOOK:
        draw_choice(right, y, config->look == LOOK_CLASSIC ? "Classic" : "Fresh", selected, text);
        break;
    case ITEM_AUDIO_PACKETS:
        /* 10 and 5 carry their warning in the value itself, selected or not. */
        snprintf(value, sizeof value, config->audio_packet_ms < 20 ? "%d ms \xC2\xB7 hangs Wi-Fi" : "%d ms",
                 config->audio_packet_ms);
        draw_choice(right, y, value, selected, config->audio_packet_ms < 20 ? THEME_WARN : text);
        break;
    case ITEM_PACKET_SIZE:
        snprintf(value, sizeof value, "%d bytes", config->packet_size);
        draw_choice(right, y, value, selected, text);
        break;
    case ITEM_SLICES:
        snprintf(value, sizeof value, "%d", config->slices);
        draw_choice(right, y, value, selected, text);
        break;
    case ITEM_CODEC:
        draw_choice(right, y, config->codec == CODEC_MPEG2 ? "MPEG-2 (beta)" : "H.264", selected,
                    config->codec == CODEC_MPEG2 ? THEME_WARN : text);
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
    case ITEM_FAST_DECODE: return "Skip deblocking";
    case ITEM_LOW_LATENCY: return "Low-latency display";
    case ITEM_STATS: return "Stats";
    case ITEM_AUDIO_ON_HOST: return "Sound plays on";
    case ITEM_AUDIO_PACKETS: return "Audio packets";
    case ITEM_PACKET_SIZE: return "Video packet size";
    case ITEM_SLICES: return "Slices per frame";
    case ITEM_CODEC: return "Video codec";
    case ITEM_DECODE: return "Decode video";
    case ITEM_DRAW: return "Draw video";
    case ITEM_LOGGING: return "Logging";
    case ITEM_SOUNDS: return "Menu sounds";
    case ITEM_AUTO_RESUME: return "Reconnect on start";
    case ITEM_HUD_CORNER: return "Stats position";
    case ITEM_HUD_SIZE: return "Stats size";
    case ITEM_RUMBLE: return "Rumble";
    case ITEM_SHOW_RECENT: return "Recent apps";
    case ITEM_MENU_COMBO: return "Stream menu";
    case ITEM_WAKE: return "Wake the PC";
    case ITEM_VIDEO_TEST: return "Video test";
    case ITEM_THEME: return "Theme";
    case ITEM_LOOK: return "Look";
    case ITEM_PLAY_PICTURE: return "Play picture";
    case ITEM_FORGET: return "Forget this PC";
    case ITEM_APP: return "App";
    case ITEM_HOST: return "Host PC";
    default: return "";
    }
}

static Icon item_icon(int item)
{
    switch (item) {
    case ITEM_APP: return ICON_APP;
    case ITEM_HOST: return ICON_HOST;
    case ITEM_RESOLUTION: return ICON_RESOLUTION;
    case ITEM_FPS: return ICON_FPS;
    case ITEM_BITRATE: return ICON_BITRATE;
    case ITEM_FAST_DECODE: return ICON_FAST;
    case ITEM_LOW_LATENCY: return ICON_LATENCY;
    case ITEM_STATS: return ICON_STATS;
    case ITEM_AUDIO_ON_HOST: return ICON_SPEAKER;
    case ITEM_AUDIO_PACKETS: return ICON_WAVE;
    case ITEM_PACKET_SIZE: return ICON_PACKET;
    case ITEM_SLICES: return ICON_SLICES;
    case ITEM_CODEC: return ICON_DECODE;
    case ITEM_DECODE: return ICON_DECODE;
    case ITEM_DRAW: return ICON_DRAW;
    case ITEM_LOGGING: return ICON_LOG;
    case ITEM_SOUNDS: return ICON_SPEAKER;
    case ITEM_AUTO_RESUME: return ICON_RECONNECT;
    case ITEM_HUD_CORNER: return ICON_HUD_CORNER;
    case ITEM_HUD_SIZE: return ICON_STATS;
    case ITEM_RUMBLE: return ICON_RUMBLE;
    case ITEM_SHOW_RECENT: return ICON_APP;
    case ITEM_MENU_COMBO: return ICON_KEYBOARD;
    case ITEM_WAKE: return ICON_WAKE;
    case ITEM_VIDEO_TEST: return ICON_TEST;
    case ITEM_THEME: return ICON_THEME;
    case ITEM_LOOK: return ICON_THEME;
    case ITEM_PLAY_PICTURE: return ICON_THEME;
    default: return ICONS;   /* none */
    }
}

/* A row's icon, centred on cy at the row's left. */
static void draw_item_icon(int item, int32_t cy, bool selected)
{
    icon_draw(item_icon(item), CONTENT_X + 22, (float)(cy - ICON_SIZE / 2), selected ? THEME_ACCENT : THEME_TEXT_3);
}

/* The selected row opens to show its help (if details are on) and its warning under the label: wraps them into
 * help_out and warning_out (empty if none), sets *help_lines, and returns how tall that part is (0 for neither). */
static int32_t tip_height(int item, char *help_out, size_t help_size, char *warning_out, size_t warning_size,
                          int *help_lines)
{
    const char *help, *warning;
    item_help(item, &help, &warning);
    help_out[0] = warning_out[0] = 0;
    *help_lines = show_details ? gfx_wrap(help_out, help_size, help, TIP_CHARS) : 0;
    if (!*help_lines && !warning)
        return 0;
    int32_t h = *help_lines * 20 + 14;
    if (warning)
        h += gfx_wrap(warning_out, warning_size, warning, TIP_CHARS - 2) * 20 + (*help_lines ? 6 : 0);
    return h;
}

/* The About tab, under its one row: what this is, who made what it's built on, and the fonts' licences. */
static void draw_about(int32_t y)
{
    const int32_t x = CONTENT_X + 16;
    gfx_text(x, y, 2, THEME_TEXT, "Moonlight 360");
    gfx_text(x + gfx_text_width("Moonlight 360", 2) + 16, y + 14, 1, THEME_TEXT_3,
             "version 0.4  \xC2\xB7  build " __DATE__ " " __TIME__);
    y += 48;
    gfx_text(x, y, 1, THEME_TEXT_2,
             "Streams games from a PC running Sunshine, Apollo or Vibeshine to an Xbox 360 (RGH, Aurora).");
    y += 44;

    /* Who made it: a card each, a round picture (about_images.h), the name and the part they played. */
    gfx_text(x, y, 1, THEME_ACCENT_TEXT, "MADE BY");
    y += 30;
    static GfxImage *pictures[2];
    static const uint8_t *const pixels[2] = { about_creator, about_claude };
    static const char *const people[2][2] = { { "everythings-gonna-be-alright", "Creator" },
                                             { "Claude", "Co-creator, by Anthropic: code and design" } };
    for (int i = 0, cx = x; i < 2; i++) {
        if (!pictures[i])
            pictures[i] = gfx_image_create(pixels[i], ABOUT_AVATAR, ABOUT_AVATAR);
        const int32_t w = 76 + (gfx_text_width(people[i][0], 1) > gfx_text_width(people[i][1], 1)
                                    ? gfx_text_width(people[i][0], 1)
                                    : gfx_text_width(people[i][1], 1));
        gfx_rect((float)cx, (float)y, (float)w + 18.0f, 68.0f, 14.0f, THEME_SURFACE);
        gfx_image(pictures[i], (float)cx + 10.0f, (float)y + 10.0f, ABOUT_AVATAR, ABOUT_AVATAR, 0xFFFFFFFF);
        gfx_text(cx + 72, y + 12, 1, THEME_TEXT, people[i][0]);
        gfx_text(cx + 72, y + 34, 1, THEME_TEXT_3, people[i][1]);
        cx += w + 34;
    }
    y += 68 + 26;

    static const char *const credits[][2] = {
        { "BUILT ON", "moonlight-common-c and libgamestream (Moonlight, GPLv3), FFmpeg, Opus, curl, expat, mbedTLS" },
        { "", "and xsdk, a homebrew Xbox 360 SDK." },
        { "FONTS", "Ubuntu Sans: Ubuntu Font Licence 1.0, ubuntu.com/legal/font-licence" },
        { "", "Inter (arrows and triangles): SIL Open Font License 1.1, openfontlicense.org" },
        { "ICONS", "Material Symbols Rounded (Google): Apache License 2.0" },
    };
    for (int i = 0; i < COUNT(credits); i++, y += 24) {
        if (credits[i][0][0]) {
            y += i ? 10 : 0;
            gfx_text(x, y, 1, THEME_ACCENT_TEXT, credits[i][0]);
        }
        gfx_text(x + 110, y, 1, THEME_TEXT_2, credits[i][1]);
    }
    y += 18;
    gfx_text(x, y, 1, THEME_TEXT_3,
             config->logging ? "Logging is on: Y shows the log."
             : fresh()       ? "Logging is off (Settings, Advanced)."
                             : "Logging is off (Other tab).");
}

/* The About tab's log view: the newest lines, oldest at the top, errors red and warnings yellow. */
static void draw_log(void)
{
    enum { LINE_H = 21, LINES = 20 };
    const int32_t x = CONTENT_X + 16;
    gfx_text(x, CONTENT_Y - 4, 2, THEME_TEXT, "Log");
    gfx_text(x + gfx_text_width("Log", 2) + 16, CONTENT_Y + 10, 1, THEME_TEXT_3,
             "moonlight360.log, the newest lines at the bottom");
    const float top = CONTENT_Y + 44.0f, h = (float)(LINES * LINE_H + 20);
    gfx_rect(CONTENT_X, top, CONTENT_RIGHT - CONTENT_X, h, 12.0f, THEME_SURFACE);
    static char text[8192];
    logbuf_recent(text, sizeof text);
    /* The last LINES whole lines: back from the end. */
    const char *starts[LINES];
    int n = 0;
    size_t end = strlen(text);
    while (end > 0 && text[end - 1] == '\n')
        text[--end] = 0;
    for (size_t i = end; i > 0 && n < LINES; i--)
        if (text[i - 1] == '\n') {
            text[i - 1] = 0;
            starts[n++] = &text[i];
        }
    if (n < LINES && end)
        starts[n++] = text;
    if (!n) {
        gfx_text(x + 4, (int32_t)top + 14, 1, THEME_TEXT_3, "Nothing logged yet.");
        return;
    }
    for (int k = 0; k < n; k++) {   /* starts runs newest first */
        const char *line = starts[n - 1 - k];
        /* "2026-11-24 01:41:05.123 [I] ...": the time is enough here. */
        if (strlen(line) > 24 && line[4] == '-' && line[10] == ' ')
            line += 11;
        const char *tag = strchr(line, '[');
        char fitted[160];
        gfx_fit(fitted, sizeof fitted, line, 108);
        const uint32_t colour = tag && tag - line < 16 && strncmp(tag, "[E]", 3) == 0   ? THEME_ERROR
                                : tag && tag - line < 16 && strncmp(tag, "[W]", 3) == 0 ? THEME_WARN
                                                                                       : THEME_TEXT_2;
        gfx_text(x + 4, (int32_t)top + 10 + k * LINE_H, 1, colour, fitted);
    }
}

/* A small heading above row i of the tab, where a group starts, or NULL. */
static const char *section_before(int t, int i)
{
    if (tabs != classic_tabs)
        return NULL;
    if (t == TAB_VIDEO)   /* Resolution, Frame rate, Bitrate, Codec | Low-latency, Skip deblocking | Stats, position, size */
        return i == 0 ? "QUALITY" : i == 4 ? "PERFORMANCE" : i == 6 ? "STATS" : NULL;
    if (t == TAB_OTHER)
        return i == 0 ? "GENERAL" : i == 3 ? "SOUND" : i == 6 ? "APPEARANCE" : i == 8 ? "DEBUG" : NULL;
    return NULL;
}

enum { SECTION_H = 32, THEME_CARDS_H = 46 };

/* Room the selected row takes under its label line for more than a value: the Theme row's cards (they show while it's
 * selected, so the tab still fits with another row opened). */
static int32_t row_extra(int item, bool selected)
{
    return item == ITEM_THEME && selected ? THEME_CARDS_H : 0;
}

/* The themes under the Theme row: a dot of each colour, the one in use ringed. */
static void draw_theme_cards(int32_t y, bool row_selected)
{
    (void)row_selected;
    const float cy = (float)y + 22.0f;
    for (int t = 0; t < THEMES; t++) {
        const float cx = (float)LABEL_X + 14.0f + (float)t * 44.0f;
        if (&themes[t] == theme) {
            gfx_circle(cx, cy, 16.0f, THEME_TEXT);
            gfx_circle(cx, cy, 13.0f, THEME_ROW_SELECTED);
        }
        gfx_circle(cx, cy, 11.0f, themes[t].accent);
    }
}

enum { CUT_TOP = 1, CUT_BOTTOM = 2 };

/* Draws the tab's rows, scrolled (smoothly) so the selected one and what it opens to are in view; returns which
 * edges of the content area rows run past (CUT_TOP, CUT_BOTTOM), for menu_draw to cover. */
static int draw_settings_tab(void)
{
    const int count = tabs[tab].count;
    /* The rows' heights first: where the selected one is, and how tall the whole list is. */
    int32_t total = 0, sel_top = 0, sel_bottom = 0;
    for (int i = 0; i < count; i++) {
        const bool on = i == cursor[tab];
        if (section_before(tab, i))
            total += SECTION_H;
        if (on)
            sel_top = total - (section_before(tab, i) ? SECTION_H : 0);   /* its heading in view too */
        char help[512], warning[256];
        int help_lines = 0;
        const int32_t tip = on ? tip_height(tabs[tab].items[i], help, sizeof help, warning, sizeof warning,
                                            &help_lines)
                               : 0;
        total += SET_ROW_H + row_extra(tabs[tab].items[i], on) + tip;
        if (on)
            sel_bottom = total;
    }
    const int32_t visible = THEME_PANEL_BOTTOM - CONTENT_Y;
    static float scroll[TABS_MAX];
    float target = scroll[tab];
    if (sel_bottom - target > visible)
        target = (float)(sel_bottom - visible);
    if (sel_top < target)
        target = (float)sel_top;
    const float most = total > visible ? (float)(total - visible) : 0.0f;
    target = target < 0.0f ? 0.0f : target > most ? most : target;
    scroll[tab] = approach(scroll[tab], target);
    const int32_t top = CONTENT_Y - (int32_t)(scroll[tab] + 0.5f);

    int32_t y = top;
    /* The selected row's place and height first: its backdrop goes under every row. */
    {
        int32_t sy = top;
        for (int i = 0; i < count; i++) {
            if (section_before(tab, i))
                sy += SECTION_H;
            char help[512], warning[256];
            int help_lines = 0;
            const int32_t tip = i == cursor[tab] ? tip_height(tabs[tab].items[i], help, sizeof help, warning,
                                                              sizeof warning, &help_lines)
                                                 : 0;
            const int32_t extra = row_extra(tabs[tab].items[i], i == cursor[tab]);
            if (i == cursor[tab])
                draw_selection(LIST_SETTINGS + tab * 16, CONTENT_X, (float)sy, CONTENT_RIGHT - CONTENT_X,
                               (float)(SET_ROW_H + extra + tip));
            sy += SET_ROW_H + extra + tip;
        }
    }
    for (int i = 0; i < count; i++) {
        const int item = tabs[tab].items[i];
        const bool on = i == cursor[tab];
        const char *section = section_before(tab, i);
        if (section) {   /* a heading, with room above it after the group before */
            gfx_text(CONTENT_X + 16, y + (i ? 8 : 2), 1, THEME_ACCENT_TEXT, section);
            y += SECTION_H;
        }
        char help[512], warning[256];
        int help_lines = 0;
        const int32_t tip = on ? tip_height(item, help, sizeof help, warning, sizeof warning, &help_lines) : 0;
        const int32_t extra = row_extra(item, on);
        /* No divider under a group's last row. */
        draw_row(CONTENT_X, (float)y, CONTENT_RIGHT - CONTENT_X, (float)(SET_ROW_H + extra + tip), on,
                 i == count - 1 || i + 1 == cursor[tab] || section_before(tab, i + 1));
        const int32_t text_y = y + (SET_ROW_H - 40) / 2, cy = y + SET_ROW_H / 2;
        draw_item_icon(item, cy, on);
        gfx_text(LABEL_X, text_y, 2, on ? THEME_TEXT : THEME_TEXT_2, item_label(item));
        draw_value(item, CONTENT_RIGHT - 24, text_y, cy, on);
        if (extra)
            draw_theme_cards(y + SET_ROW_H - 2, on);
        if (on) {
            int32_t ty = y + SET_ROW_H + extra - 4;
            gfx_text(LABEL_X, ty, 1, THEME_TEXT_2, help);
            if (help_lines)
                ty += help_lines * 20 + 6;
            if (warning[0]) {
                gfx_circle(LABEL_X + 5.0f, (float)ty + 10.0f, 5.0f, THEME_WARN);
                gfx_text(LABEL_X + 20, ty, 1, THEME_WARN, warning);
            }
        }
        y += SET_ROW_H + extra + tip;
    }
    if (tabs[tab].count == 0 && about_log && config->logging)
        draw_log();
    else if (tabs[tab].count == 0)
        draw_about(CONTENT_Y + 8);
    return (scroll[tab] > 0.5f ? CUT_TOP : 0) | ((float)total - scroll[tab] > (float)visible + 0.5f ? CUT_BOTTOM : 0);
}

/* The Play tab's covers as textures, by app id (coverart.c loads their pixels); dropped if the host changes. */
static GfxImage *play_cover(int id)
{
    enum { CACHED = 6 };
    static struct {
        int id;
        GfxImage *image;
        uint32_t used;
    } cache[CACHED];
    static char cache_host[64];
    if (id <= 0)
        return NULL;
    if (strcmp(cache_host, config->host) != 0) {
        for (int i = 0; i < CACHED; i++) {
            gfx_image_free(cache[i].image);
            cache[i].image = NULL;
            cache[i].id = 0;
        }
        snprintf(cache_host, sizeof cache_host, "%s", config->host);
    }
    int oldest = 0;
    for (int i = 0; i < CACHED; i++) {
        if (cache[i].id == id && cache[i].image) {
            cache[i].used = gfx_frame();
            return cache[i].image;
        }
        if (cache[i].used < cache[oldest].used)
            oldest = i;
    }
    const uint8_t *pixels = coverart_get(config->host, id);
    if (!pixels)
        return NULL;
    gfx_image_free(cache[oldest].image);
    cache[oldest].image = gfx_image_create_rounded(pixels, APPS_COVER_W, APPS_COVER_H, COVER_RADIUS);
    cache[oldest].id = id;
    cache[oldest].used = gfx_frame();
    return cache[oldest].image;
}

/* The Play tab's right side: the moon, and under it the Recent row: the last apps streamed as covers, a ring that
 * slides to the one the cursor is on, a dot on the app chosen now, and the focused one's name under the row. */
static void draw_play_covers(void)
{
    enum { TILE_W = 90, TILE_H = 120, TILE_GAP = 14, LIFT = 8 };
    moon_draw(960.0f, 300.0f, 100.0f);
    const int n = recent_count();
    if (!n)
        return;
    const float cx = 960.0f;
    const float row_w = CONFIG_RECENT * TILE_W + (CONFIG_RECENT - 1) * TILE_GAP;
    const float left = cx - row_w / 2.0f, ty = 462.0f;
    gfx_text((int32_t)left, (int32_t)ty - 28, 1, THEME_ACCENT_TEXT, "RECENT");

    /* The ring: where the focused tile is, sliding there; it fades in and out as the row gains and loses focus. */
    static float ring_x = -1.0f, ring_alpha;
    const float target_x = left + (float)recent_index * (TILE_W + TILE_GAP);
    ring_x = ring_x < 0.0f ? target_x : approach(ring_x, target_x);
    ring_alpha = approach(ring_alpha * 255.0f, recent_focus ? 255.0f : 0.0f) / 255.0f;
    const float lift = LIFT * ring_alpha;
    if (ring_alpha > 0.02f) {
        const uint32_t a = (uint32_t)(ring_alpha * 255.0f) << 24;
        gfx_rect(ring_x - 5.0f, ty - lift - 5.0f, TILE_W + 10.0f, TILE_H + 10.0f, 12.5f, a | (THEME_ACCENT & 0x00FFFFFF));
    }
    for (int i = 0; i < n; i++) {
        const bool on = recent_focus && i == recent_index;
        const float x = left + (float)i * (TILE_W + TILE_GAP);
        /* The focused tile rises with the ring; on the row, the others dim so it stands out. */
        const float y = ty - (i == recent_index ? lift : 0.0f);
        if (!on)
            gfx_rect(x - 1.0f, y - 1.0f, TILE_W + 2.0f, TILE_H + 2.0f, 8.5f, THEME_BORDER);
        GfxImage *image = play_cover(config->recent_id[i]);
        const uint32_t tint = !recent_focus || on ? 0xFFFFFFFF : 0xFF7A7A7A;
        if (image) {
            gfx_image(image, x, y, TILE_W, TILE_H, tint);
        } else {
            gfx_rect(x, y, TILE_W, TILE_H, 7.5f, THEME_SURFACE);
            const char initial[2] = { config->recent_name[i][0] ? config->recent_name[i][0] : '?', 0 };
            gfx_text_center((int32_t)(x + TILE_W / 2), (int32_t)(y + TILE_H / 2 - 20), 2,
                            on ? THEME_ACCENT_TEXT : THEME_TEXT_3, initial);
        }
        if (!strcmp(config->recent_name[i], config->app)) {   /* the app chosen now */
            gfx_circle(x + 10.0f, y + 10.0f, 6.0f, THEME_BG);
            gfx_circle(x + 10.0f, y + 10.0f, 4.0f, THEME_ACCENT);
        }
    }
    if (recent_focus && recent_index < n) {   /* the focused one's name, and what A does */
        char line[64];
        gfx_fit(line, sizeof line, config->recent_name[recent_index], 30);
        const int32_t ny = (int32_t)(ty + TILE_H) + 12;
        gfx_text((int32_t)left, ny, 1, THEME_TEXT, line);
    }
}

static void draw_play_tab(void)
{
    /* On the Recent row, nothing on the left is selected. */
    const int item = recent_focus ? -1 : selected_item();
    char line[160];
    const int32_t x = CONTENT_X + 16;
    int32_t y = CONTENT_Y + 6;

    draw_play_covers();

    if (config->host[0]) {
        gfx_text(x, y, 1, THEME_ACCENT_TEXT, "READY TO STREAM");
        y += 30;
        gfx_fit_width(line, sizeof line, config->app, 3, 700);   /* up to the moon's halo */
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

    /* The main button. Pressed, it dips for a quarter of the time, then springs back a little past its size while a
     * glow of its colour spreads from it, a flash fades off it and a band of light sweeps across it; its arrow shoots
     * off to the right and comes back in from the left. */
    const float bx = (float)x, by = (float)y, bw = 440.0f, bh = 60.0f;
    const bool on = item == ITEM_CONNECT;
    const float launch = launching ? (float)(gfx_frame() - launch_frame) / LAUNCH_FRAMES : -1.0f;
    const float dip = 0.25f, spring = launch < dip ? 0.0f : (launch > 1.0f ? 1.0f : launch - dip) / (1.0f - dip);
    if (launch >= 0.0f) {
        const float down = launch < dip ? launch / dip : 1.0f, back = spring - 1.0f;
        /* Down eases out; back overshoots (the "back" curve, a little more than the standard one), ending at 1. */
        const float size = launch < dip ? 1.0f - 0.08f * (1.0f - (1.0f - down) * (1.0f - down))
                                        : 0.92f + 0.08f * (1.0f + 3.2f * back * back * back + 2.2f * back * back);
        if (launch >= dip) {
            const float spread = 8.0f + 26.0f * spring;
            const uint32_t glow = (uint32_t)(150.0f * (1.0f - spring)) << 24 | (THEME_ACCENT & 0x00FFFFFF);
            gfx_shadow(bx, by, bw, bh, bh / 2, spread, glow);
        }
        gfx_scale_begin(size, bx + bw / 2, by + bh / 2);
    }
    if (on) {
        gfx_rect(bx, by, bw, bh, bh / 2, THEME_ACCENT);
    } else {
        gfx_rect(bx, by, bw, bh, bh / 2, THEME_BORDER);
        gfx_rect(bx + 1.0f, by + 1.0f, bw - 2.0f, bh - 2.0f, bh / 2 - 1.0f, THEME_BG);
    }
    /* After a failure, the same button tries again. */
    const char *label = !config->host[0]                           ? "Find your PC"
                        : message[0] && message_is_error(message) ? "\xE2\x86\xBB  Try again"
                                                                  : ARROW_RIGHT "  Start streaming";
    const uint32_t ink = on ? THEME_ON_ACCENT : THEME_TEXT_2;
    if (launch >= 0.0f && label[0] == ARROW_RIGHT[0]) {
        /* The arrow apart from its words: off to the right, fading, over the first half; back in from the left. */
        const int32_t arrow_w = gfx_text_width(ARROW_RIGHT, 2), left = (int32_t)(bx + bw / 2) - gfx_text_width(label, 2) / 2;
        const float out = launch < 0.5f ? launch / 0.5f : 0.0f, in = launch < 0.5f ? 0.0f : (launch - 0.5f) / 0.5f;
        const float dx = launch < 0.5f ? 46.0f * out * out : -22.0f * (1.0f - in) * (1.0f - in);
        const float seen = launch < 0.5f ? 1.0f - out : in;
        gfx_text(left + (int32_t)dx, (int32_t)by + 10, 2, (uint32_t)((float)(ink >> 24) * seen) << 24 | (ink & 0x00FFFFFF),
                 ARROW_RIGHT);
        gfx_text(left + arrow_w, (int32_t)by + 10, 2, ink, label + strlen(ARROW_RIGHT));
    } else
        gfx_text_center((int32_t)(bx + bw / 2), (int32_t)by + 10, 2, ink, label);
    if (launch >= 0.0f) {
        if (launch >= dip)
            gfx_rect(bx, by, bw, bh, bh / 2, (uint32_t)(55.0f * (1.0f - spring)) << 24 | 0x00FFFFFF);
        /* The sweep: a soft band of light, left to right, kept to the button's straight middle (its ends are round). */
        const float sweep = (launch - 0.15f) / 0.6f;
        if (sweep > 0.0f && sweep < 1.0f) {
            enum { STRIPS = 9 };
            const float band = 64.0f, strip = band / STRIPS, from = bx + bh / 2, to = bx + bw - bh / 2;
            const float centre = from - band / 2 + (to - from + band) * sweep;
            for (int i = 0; i < STRIPS; i++) {
                const float sx = centre - band / 2 + strip * (float)i, ex = sx + strip;
                const float l = sx < from ? from : sx, r = ex > to ? to : ex;
                if (r <= l)
                    continue;
                const float peak = 1.0f - fabsf(((float)i + 0.5f) / STRIPS * 2.0f - 1.0f);   /* 0 at the edges, 1 mid */
                gfx_rect(l, by, r - l, bh, 0.0f, (uint32_t)(70.0f * peak * peak) << 24 | 0x00FFFFFF);
            }
        }
        gfx_scale_end();
    }
    y += (int32_t)bh + (message[0] ? 16 : 36);   /* closer with a message, so its box has room under them */

    /* App and host. */
    static const int rows[] = { ITEM_APP, ITEM_HOST };
    const float rw = 600.0f;
    for (int i = 0; i < 2; i++)
        if (item == rows[i])
            draw_selection(LIST_PLAY, CONTENT_X, (float)(y + i * ROW_H), rw, ROW_H);
    for (int i = 0; i < 2; i++, y += ROW_H) {
        const bool sel = item == rows[i];
        draw_row(CONTENT_X, (float)y, rw, ROW_H, sel, i == 1 || item == rows[1]);
        const int32_t ty = y + (ROW_H - 40) / 2;
        draw_item_icon(rows[i], y + ROW_H / 2, sel);
        gfx_text(LABEL_X, ty, 2, sel ? THEME_TEXT : THEME_TEXT_2, item_label(rows[i]));
        const char *value = rows[i] == ITEM_APP ? config->app : config->host[0] ? config->host : "Not set";
        const int32_t right = CONTENT_X + (int32_t)rw - 24;
        /* As wide as fits between the label and the chevron. */
        gfx_fit_width(line, sizeof line, value, 2,
                      right - 30 - (LABEL_X + gfx_text_width(item_label(rows[i]), 2) + 24));
        gfx_text(right - gfx_text_width("\xE2\x80\xBA", 2), ty, 2, sel ? THEME_ACCENT : THEME_TEXT_3,
                 "\xE2\x80\xBA");   /* U+203A */
        gfx_text_right(right - 30, ty, 2,
                       rows[i] == ITEM_HOST && !config->host[0] ? THEME_WARN : sel ? THEME_TEXT : THEME_TEXT_2, line);
    }

    /* The last session's outcome: for a known failure, what it means and what to do, then the message itself. */
    if (message[0]) {
        const bool error = message_is_error(message);
        const char *title, *advice;
        friendly_error(message, &title, &advice);
        char wrapped[640], raw[700], one_line[640];
        /* The original message as one paragraph (its line breaks as spaces). */
        size_t k = 0;
        for (const char *c = message; *c && k + 1 < sizeof one_line; c++)
            one_line[k++] = *c == '\n' ? ' ' : *c;
        one_line[k] = 0;
        /* Left of the Recent row when it's there (an error hides it: recent_count), else the full width; all of
         * the text, wrapped, the box growing up to fit it. */
        const int32_t box_right = recent_count() ? 780 : CONTENT_RIGHT;
        const int chars = (box_right - CONTENT_X - 60) / 9;   /* Ubuntu Sans averages under 9 pixels a character */
        /* As many lines as fit under the rows (five); only a very long message (a host's whole app list) is cut. */
        enum { MAX_LINES = 5 };
        int advice_lines = 0, raw_lines = 0, lines;
        if (title) {
            advice_lines = gfx_wrap(wrapped, sizeof wrapped, advice, chars);
            raw_lines = gfx_wrap(raw, sizeof raw, one_line, chars);
            const int room = MAX_LINES - 1 - advice_lines < 1 ? 1 : MAX_LINES - 1 - advice_lines;
            if (raw_lines > room) {   /* cut after the last line that fits, with an ellipsis */
                char *cut = raw;
                for (int i = 0; i < room && cut; i++)
                    cut = strchr(cut + (i ? 1 : 0), '\n');
                if (cut)
                    snprintf(cut, sizeof raw - (size_t)(cut - raw), "\xE2\x80\xA6");
                raw_lines = room;
            }
            lines = 1 + advice_lines + raw_lines;
        } else {
            lines = gfx_wrap(wrapped, sizeof wrapped, message, chars);
            if (lines > MAX_LINES) {
                char *cut = wrapped;
                for (int i = 0; i < MAX_LINES && cut; i++)
                    cut = strchr(cut + (i ? 1 : 0), '\n');
                if (cut)
                    snprintf(cut, sizeof wrapped - (size_t)(cut - wrapped), "\xE2\x80\xA6");
                lines = MAX_LINES;
            }
        }
        const float mh = (float)(lines * 20 + 24 + (title ? 6 : 0)), my = THEME_PANEL_BOTTOM - mh + 6.0f;
        gfx_rect(CONTENT_X, my, (float)(box_right - CONTENT_X), mh, 12.0f, error ? THEME_ERROR_TINT : THEME_ACCENT_TINT);
        gfx_circle(CONTENT_X + 26, my + 22.0f, 6.0f, error ? THEME_ERROR : THEME_ACCENT);
        if (title) {
            int32_t ty = (int32_t)my + 12;
            gfx_text(CONTENT_X + 46, ty, 1, error ? THEME_ERROR_TEXT : THEME_TEXT, title);
            ty += 22;
            gfx_text(CONTENT_X + 46, ty, 1, THEME_TEXT_2, wrapped);
            ty += advice_lines * 20 + 4;
            gfx_text(CONTENT_X + 46, ty, 1, THEME_TEXT_3, raw);
        } else {
            gfx_text(CONTENT_X + 46, (int32_t)my + 12, 1, error ? THEME_ERROR_TEXT : THEME_TEXT_2, wrapped);
        }
    } else {
        char tip[96];
        snprintf(tip, sizeof tip, "While streaming, hold %s for a second to open the stream menu.",
                 menu_combo_name(config->menu_combo));
        gfx_text(x, THEME_PANEL_BOTTOM - 20, 1, THEME_TEXT_3, tip);
    }
}

static void draw_finder(void)
{
    char line[96];
    const int32_t x = CONTENT_X + 16;
    gfx_text(x, CONTENT_Y, 2, THEME_TEXT, "Find your PC");
    gfx_text(x + gfx_text_width("Find your PC", 2) + 20, CONTENT_Y + 14, 1, THEME_TEXT_3,
             "PCs running Sunshine or Apollo on your network");
    int tried, total;
    discover_progress(&tried, &total);
    const bool running = discover_running();
    const int n = discover_count();
    if (running) {
        snprintf(line, sizeof line, "Searching\xE2\x80\xA6 %d%%", tried * 100 / (total ? total : 1));
        gfx_spinner(CONTENT_RIGHT - 34.0f - gfx_text_width(line, 1), CONTENT_Y + 24.0f, 9.0f, THEME_ACCENT);
    } else {
        snprintf(line, sizeof line, n == 1 ? "1 PC found" : "%d PCs found", n);
    }
    gfx_text_right(CONTENT_RIGHT - 16, CONTENT_Y + 14, 1, THEME_TEXT_3, line);

    const int rows = n + 1;
    const float top = PICKER_Y - 10.0f;
    draw_selection(LIST_FINDER, CONTENT_X, top + (float)finder_selected * ROW_H, CONTENT_RIGHT - CONTENT_X, ROW_H);
    for (int i = 0; i < rows && i < 7; i++) {
        const float y = top + (float)i * ROW_H;
        const bool on = i == finder_selected, typing = i == rows - 1;
        draw_row(CONTENT_X, y, CONTENT_RIGHT - CONTENT_X, ROW_H, on, typing || i + 1 == finder_selected);
        const int32_t ty = (int32_t)y + (ROW_H - 40) / 2, cy = (int32_t)y + ROW_H / 2;
        icon_draw(typing ? ICON_KEYBOARD : ICON_HOST, CONTENT_X + 22, (float)(cy - ICON_SIZE / 2),
                  on ? THEME_ACCENT : THEME_TEXT_3);
        if (typing) {
            gfx_text(LABEL_X, ty, 2, on ? THEME_TEXT : THEME_TEXT_2, "Type an address");
            continue;
        }
        char address[16], name[64];
        discover_get(i, address, sizeof address, name, sizeof name);
        gfx_fit(line, sizeof line, name, 30);
        gfx_text(LABEL_X, ty, 2, on ? THEME_TEXT : THEME_TEXT_2, line);
        const bool current = !strcmp(address, config->host);
        gfx_text_right(CONTENT_RIGHT - 24, ty + 10, 1, current ? THEME_GOOD : THEME_TEXT_3,
                       current ? "Current" : address);
        if (current)
            gfx_text_right(CONTENT_RIGHT - 24 - gfx_text_width("Current", 1) - 16, ty + 10, 1, THEME_TEXT_3, address);
    }
    if (!running && n == 0)
        gfx_text(x, (int32_t)(top + ROW_H) + 24, 1, THEME_TEXT_3,
                 "No PC answered. Is Sunshine running on the PC, on the same network as the console? "
                 "Y searches again.");
}

/* An app's tile: its cover, or its initial while there's none; dim unless selected; "Running" on it if it is. */
static void draw_cover(int i, float x, float y, float w, float h, bool on, float alpha)
{
    char name[64];
    bool running;
    apps_get(i, name, sizeof name, &running);
    const uint32_t a = (uint32_t)(alpha * 255.0f + 0.5f) << 24;
    if (on)   /* the outline, a little outside the cover */
        gfx_rect(x - 5.0f, y - 5.0f, w + 10.0f, h + 10.0f, 19.0f, a | (THEME_ACCENT & 0x00FFFFFF));
    if (cover_images[i]) {
        gfx_image(cover_images[i], x, y, w, h, a | (on ? 0xFFFFFF : 0x8A8A8A));
    } else {
        gfx_rect(x, y, w, h, 15.0f, a | (THEME_SURFACE & 0x00FFFFFF));
        gfx_rect(x, y, w, 4.0f, 2.0f, a | ((on ? THEME_ACCENT : THEME_ACCENT_SOFT) & 0x00FFFFFF));
        const char initial[2] = { name[0] ? name[0] : '?', 0 };
        gfx_text_center((int32_t)(x + w / 2), (int32_t)(y + h / 2 - 40), 4,
                        a | ((on ? THEME_ACCENT_TEXT : THEME_TEXT_3) & 0x00FFFFFF), initial);
    }
    if (running) {
        const int32_t tw = gfx_text_width("Running", 1);
        const float bw = (float)tw + 30.0f, bx = x + w - bw - 8.0f, by = y + 8.0f;
        gfx_rect(bx, by, bw, 26.0f, 13.0f, a | 0x0C0C0E);
        gfx_circle(bx + 13.0f, by + 13.0f, 4.0f, a | (THEME_GOOD & 0x00FFFFFF));
        gfx_text((int32_t)bx + 22, (int32_t)by + 3, 1, a | (THEME_GOOD & 0x00FFFFFF), "Running");
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
        gfx_spinner(cx, 340.0f, 22.0f, THEME_ACCENT);
        gfx_text_center((int32_t)cx, 390, 1, THEME_TEXT_2, "Getting the app list from the PC\xE2\x80\xA6");
        return;
    }
    if (st == APPS_FAILED) {
        char text[256], wrapped[300];
        apps_message(text, sizeof text);
        gfx_wrap(wrapped, sizeof wrapped, text, HELP_CHARS - 6);
        gfx_rect(CONTENT_X, PICKER_Y, CONTENT_RIGHT - CONTENT_X, 64.0f, 12.0f, THEME_ERROR_TINT);
        gfx_circle(CONTENT_X + 26, PICKER_Y + 32.0f, 6.0f, THEME_ERROR);
        gfx_text(CONTENT_X + 46, PICKER_Y + 12, 1, THEME_ERROR_TEXT, wrapped);
        return;
    }
    snprintf(line, sizeof line, "%d / %d", picker_selected + 1, n);
    gfx_text_right(CONTENT_RIGHT - 16, CONTENT_Y + 14, 1, THEME_TEXT_3, line);

    /* Covers as they arrive: a texture each, two a frame at most so a burst doesn't stall a frame. */
    if (apps_generation() != cover_generation) {
        for (int i = 0; i < APPS_MAX; i++) {
            gfx_image_free(cover_images[i]);
            cover_images[i] = NULL;
        }
        cover_generation = apps_generation();
    }
    for (int i = 0, made = 0; i < n && made < 2; i++) {
        const uint8_t *pixels = cover_images[i] ? NULL : apps_cover(i);
        if (pixels) {
            cover_images[i] = gfx_image_create_rounded(pixels, APPS_COVER_W, APPS_COVER_H, COVER_RADIUS);
            made++;
        }
    }

    /* The row scrolls (smoothly) to keep the selection in view; tiles past its ends fade out. */
    static float scroll = -1.0f;
    const float pitch = COVER_W + COVER_GAP;
    const float row_w = PICKER_VISIBLE * COVER_W + (PICKER_VISIBLE - 1) * COVER_GAP;
    const float left = cx - row_w / 2.0f, right = left + row_w;
    const float target = (float)picker_top * pitch;
    scroll = scroll < 0.0f ? target : approach(scroll, target);
    const float top = PICKER_Y + 20.0f;
    for (int i = 0; i < n; i++) {
        float tx = left + (float)i * pitch - scroll;
        if (tx + COVER_W < left - pitch || tx > right + pitch)
            continue;
        const float outside = tx < left ? left - tx : tx + COVER_W > right ? tx + COVER_W - right : 0.0f;
        const float alpha = outside <= 0.0f ? 1.0f : outside >= 120.0f ? 0.0f : 1.0f - outside / 120.0f;
        if (alpha <= 0.0f)
            continue;
        const bool on = i == picker_selected;
        float w = COVER_W, h = COVER_H, ty = top;
        if (on) {   /* raised and a little bigger */
            w = COVER_W * 1.06f;
            h = COVER_H * 1.06f;
            tx -= (w - COVER_W) / 2.0f;
            ty -= 14.0f;
        }
        draw_cover(i, tx, ty, w, h, on, alpha);
    }
    if (picker_top > 0)
        gfx_text_center((int32_t)left - 30, (int32_t)(top + COVER_H / 2 - 10), 1, THEME_TEXT_3, ARROW_LEFT);
    if (picker_top + PICKER_VISIBLE < n)
        gfx_text_center((int32_t)right + 30, (int32_t)(top + COVER_H / 2 - 10), 1, THEME_TEXT_3, ARROW_RIGHT);

    /* The selected app, under the row. */
    char name[64];
    bool running;
    apps_get(picker_selected, name, sizeof name, &running);
    gfx_fit(line, sizeof line, name, 44);
    const int32_t ny = (int32_t)(top + COVER_H) + 34;
    gfx_text((int32_t)left, ny, 2, THEME_TEXT, line);
    gfx_text((int32_t)left, ny + 46, 1, running ? THEME_GOOD : THEME_TEXT_3,
             running ? "Running on the PC: A picks it, and Connect resumes it." : "A picks it for Connect.");
}

void menu_draw(uint32_t width, uint32_t height)
{
    if (fresh()) {
        fresh_draw(width, height);
        return;
    }
    ui_draw_background(width, height);
    PerfReadings perf;
    perf_get(&perf);
    if (finder_open) {
        ui_draw_header();
        ui_draw_temperatures(&perf);
        draw_finder();
        static const char *const labels[] = { "Choose", "Back", "Type an address", "Search again" };
        ui_draw_hints("ABXY", labels);
        return;
    }
    if (picker_open) {
        ui_draw_header();
        ui_draw_temperatures(&perf);
        draw_picker();
        static const char *const labels[] = { "Choose", "Back", "Type a name", "Refresh" };
        ui_draw_hints("ABXY", labels);
        return;
    }
    /* The content first; if a long tab scrolled past its area, the background covers what's outside it, and the
     * header, the tabs and the footer go over that. */
    const int cut = tab == TAB_PLAY ? (draw_play_tab(), 0) : draw_settings_tab();
    /* Each cut edge: covered past the content area, and a short fade into it, in flat bands (gfx_rect_v's
     * gradients are what the console drew wrongly). */
    static const uint8_t fade[] = { 0xD0, 0x90, 0x58, 0x28 };
    if (cut & CUT_TOP) {
        gfx_rect(0.0f, 0.0f, (float)width, CONTENT_Y - 2.0f, 0.0f, THEME_BG);
        for (int i = 0; i < 4; i++)
            gfx_rect(0.0f, CONTENT_Y - 2.0f + (float)(i * 6), (float)width, 6.0f, 0.0f,
                     (uint32_t)fade[i] << 24 | (THEME_BG & 0x00FFFFFF));
    }
    if (cut & CUT_BOTTOM) {
        gfx_rect(0.0f, THEME_PANEL_BOTTOM, (float)width, (float)height - THEME_PANEL_BOTTOM, 0.0f, THEME_BG);
        for (int i = 0; i < 4; i++)
            gfx_rect(0.0f, THEME_PANEL_BOTTOM - (float)((i + 1) * 6), (float)width, 6.0f, 0.0f,
                     (uint32_t)fade[i] << 24 | (THEME_BG & 0x00FFFFFF));
    }
    ui_draw_header();
    ui_draw_temperatures(&perf);
    draw_tabs();
    /* A new tab fades in: the background over it, clearing over FADE_FRAMES. */
    static int drawn_tab = -1;
    static uint32_t tab_since;
    if (tab != drawn_tab) {
        tab_since = gfx_frame();
        if (drawn_tab < 0)
            tab_since -= FADE_FRAMES;   /* the first frame: no fade */
        drawn_tab = tab;
    }
    const uint32_t age = gfx_frame() - tab_since;
    if (age < FADE_FRAMES) {
        const uint32_t alpha = 255u * (FADE_FRAMES - age) / FADE_FRAMES;
        gfx_rect(0.0f, CONTENT_Y - 16.0f, (float)width, THEME_PANEL_BOTTOM - CONTENT_Y + 28.0f, 0.0f,
                 alpha << 24 | (THEME_BG & 0x00FFFFFF));
    }
    const int item = selected_item();
    if (tab == TAB_PLAY && item == ITEM_APP) {
        static const char *const labels[] = { "Choose", "Type a name", NULL, "Settings", "Exit" };
        ui_draw_hints("AXLRK", labels);
    } else if (tab == TAB_PLAY && recent_focus) {
        static const char *const labels[] = { "Stream it", "Choose", "Remove", "Back" };
        ui_draw_hints("AYXB", labels);
    } else if (tab == TAB_PLAY) {
        static const char *const labels[] = { "Select", NULL, "Settings", "Exit" };
        ui_draw_hints("ALRK", labels);
    } else if (tabs[tab].count == 0 && config->logging) {
        const char *const labels[] = { "Back", about_log ? "Credits" : "Show log", NULL, "Tabs" };
        ui_draw_hints("BYLR", labels);
    } else if (tabs[tab].count == 0) {
        static const char *const labels[] = { "Back", NULL, "Tabs" };
        ui_draw_hints("BLR", labels);
    } else {
        const char *const labels[] = { is_toggle(item) ? "Toggle" : "Change", "Back",
                                       show_details ? "Hide details" : "Details", NULL, "Tabs" };
        ui_draw_hints(is_toggle(item) ? "ABYLR" : "DBYLR", labels);
    }
}

/* Fresh: drawn and driven from here, with the statics above. */
#include "menu_fresh.inc"
