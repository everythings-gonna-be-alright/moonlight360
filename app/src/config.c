#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

bool config_load(const char *path, Config *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->app, "Desktop");
    c->width = 1280;
    c->height = 720;
    c->fps = 60;   /* 720p60 by default: the console decodes it comfortably */
    c->bitrate_kbps = 8000;
    c->stats_mode = STATS_COMPACT;
    c->decode = true;
    c->draw = true;
    c->audio_output = true;
    c->keepalive_pps = 0;
    c->input = true;
    /* 20 ms, not Moonlight's 5: the host's default audio stream is ~300 packets a second of under 90 bytes, and
     * within a second of receiving it the console's Wi-Fi stops receiving anything until a reboot (2026-11-22:
     * with audio withheld a stream ran fine; it died the moment audio was let through; with 20 ms packets,
     * ~75 a second, it ran). */
    c->audio_packet_ms = 20;
    c->audio_on_host = false;
    c->low_latency = true;
    c->fast_decode = false;
    c->net_priority = 2;
    c->decode_priority = 0;
    c->packet_size = 1392;
    c->slices = 24;
    c->codec = CODEC_H264;
    c->logging = false;
    c->ui_sounds = true;
    c->theme = 0;
    c->auto_resume = false;
    c->show_recent = true;
    c->menu_combo = COMBO_BACK;
    c->hud_corner = HUD_TOP_LEFT;
    c->hud_size = HUD_MEDIUM;
    c->rumble = true;
    c->pointer_speed = 1;
    c->wake_on_lan = false;
    c->host_mac[0] = 0;
    c->app_id = 0;
    for (int i = 0; i < CONFIG_RECENT; i++) {
        c->recent_id[i] = 0;
        c->recent_name[i][0] = 0;
    }

    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(s), *value = trim(eq + 1);
        if (!strcmp(key, "host"))
            snprintf(c->host, sizeof c->host, "%s", value);
        else if (!strcmp(key, "app"))
            snprintf(c->app, sizeof c->app, "%s", value);
        else if (!strcmp(key, "width"))
            c->width = atoi(value);
        else if (!strcmp(key, "height"))
            c->height = atoi(value);
        else if (!strcmp(key, "fps"))
            c->fps = atoi(value);
        else if (!strcmp(key, "bitrate"))
            c->bitrate_kbps = atoi(value);
        else if (!strcmp(key, "stats"))
        {
            const int mode = atoi(value);
            c->stats_mode = mode < STATS_OFF ? STATS_OFF : mode >= STATS_MODES ? STATS_DETAILED : mode;
        }
        else if (!strcmp(key, "fast_decode"))
            c->fast_decode = atoi(value) != 0;
        else if (!strcmp(key, "decode"))
            c->decode = atoi(value) != 0;
        else if (!strcmp(key, "draw"))
            c->draw = atoi(value) != 0;
        else if (!strcmp(key, "logging"))
            c->logging = atoi(value) != 0;
        else if (!strcmp(key, "sounds"))
            c->ui_sounds = atoi(value) != 0;
        else if (!strcmp(key, "theme"))
            c->theme = atoi(value);
        else if (!strcmp(key, "auto_resume"))
            c->auto_resume = atoi(value) != 0;
        else if (!strcmp(key, "show_recent"))
            c->show_recent = atoi(value) != 0;
        else if (!strcmp(key, "menu_combo")) {
            const int v = atoi(value);
            c->menu_combo = v >= 0 && v < MENU_COMBOS ? v : COMBO_BACK;
        }
        else if (!strcmp(key, "hud_corner")) {
            const int v = atoi(value);
            c->hud_corner = v >= 0 && v < HUD_CORNERS ? v : HUD_TOP_LEFT;
        } else if (!strcmp(key, "hud_size")) {
            const int v = atoi(value);
            c->hud_size = v >= 0 && v < HUD_SIZES ? v : HUD_MEDIUM;
        } else if (!strcmp(key, "rumble"))
            c->rumble = atoi(value) != 0;
        else if (!strcmp(key, "picture"))
            c->picture_1080p = atoi(value) == 1080;
        else if (!strcmp(key, "pointer_speed")) {
            const int v = atoi(value);
            c->pointer_speed = v >= 0 && v < POINTER_SPEEDS ? v : 1;
        }
        else if (!strcmp(key, "wake_on_lan"))
            c->wake_on_lan = atoi(value) != 0;
        else if (!strcmp(key, "host_mac"))
            snprintf(c->host_mac, sizeof c->host_mac, "%s", value);
        else if (!strcmp(key, "app_id"))
            c->app_id = atoi(value);
        else if (!strncmp(key, "recent", 6) && key[6] >= '1' && key[6] < '1' + CONFIG_RECENT && !key[7]) {
            /* "recent1=12345|Name" */
            const int i = key[6] - '1';
            const char *bar = strchr(value, '|');
            if (bar) {
                c->recent_id[i] = atoi(value);
                snprintf(c->recent_name[i], sizeof c->recent_name[i], "%s", bar + 1);
            }
        }
        else if (!strcmp(key, "low_latency"))
            c->low_latency = atoi(value) != 0;
        else if (!strcmp(key, "audio_on_host"))
            c->audio_on_host = atoi(value) != 0;
        else if (!strcmp(key, "audio_packet_ms")) {
            int v = atoi(value);
            c->audio_packet_ms = (v == 5 || v == 10) ? v : 20;
        }
        else if (!strcmp(key, "keepalive"))
            c->keepalive_pps = atoi(value);
        else if (!strcmp(key, "packet_size"))
        {
            int v = atoi(value);
            c->packet_size = v == 1024 ? 1024 : 1392; /* 512/256 flood the Wi-Fi with tiny packets: reset */
        }
        else if (!strcmp(key, "slices")) {
            int v = atoi(value);
            c->slices = (v == 6 || v == 12 || v == 48) ? v : 24;
        }
        else if (!strcmp(key, "codec"))
            c->codec = !strcmp(value, "mpeg2") ? CODEC_MPEG2 : CODEC_H264;
    }
    fclose(f);
    return c->host[0] != '\0';
}

bool config_save(const char *path, const Config *c)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("[E] config: cannot write %s\n", path);
        return false;
    }
    fprintf(f,
            "# Moonlight 360 settings (also editable in the app)\n"
            "# host: the IP address of the PC running Sunshine\n"
            "host=%s\n"
            "# app: the Sunshine app to start, as named in its web UI\n"
            "app=%s\n"
            "width=%d\n"
            "height=%d\n"
            "fps=%d\n"
            "# kbps\n"
            "bitrate=%d\n"
            "# stats: the HUD while streaming, 0 off, 1 compact, 2 detailed\n"
            "stats=%d\n"
            "# 1 skips H.264's deblocking filter: faster decoding, slightly blockier picture\n"
            "fast_decode=%d\n"
            "# audio_packet_ms: 20, 10 or 5. Keep 20 on Wi-Fi: 5 (Moonlight's usual) kills the console's Wi-Fi\n"
            "audio_packet_ms=%d\n"
            "# audio_on_host=1 plays sound on the PC (the host still sends the audio stream)\n"
            "audio_on_host=%d\n"
            "# low_latency=1: draws each frame as soon as it is decoded (up to a refresh sooner on the TV)\n"
            "low_latency=%d\n"
            "# test switches: decode=0 receives video without decoding it, draw=0 decodes but never draws\n"
            "# packet_size: 1392 or 1024 bytes. Keep 1392 on Wi-Fi: smaller packets mean more packets per second, and\n"
            "# thousands of small packets a second make the console's Wi-Fi stop receiving until reboot\n"
            "decode=%d\n"
            "draw=%d\n"
            "packet_size=%d\n"
            "# slices: 6, 12, 24 or 48 a frame. With more slices than the 6 decode threads, a thread that finishes\n"
            "# an easy slice takes the next one, so a frame with all its detail in one place decodes faster\n"
            "slices=%d\n"
            "# codec: h264 or mpeg2. MPEG-2 is lighter to decode but needs a Sunshine host with mpeg2_mode = 2 and a\n"
            "# higher bitrate for the same picture; any other host sends H.264\n"
            "codec=%s\n"
            "# logging=1 writes moonlight360.log beside the .xex, for finding problems\n"
            "logging=%d\n"
            "# sounds=1: soft clicks in the menus\n"
            "sounds=%d\n"
            "# theme: the menus' accent colour, 0 orange, 1 blue, 2 violet, 3 pink\n"
            "theme=%d\n"
            "# auto_resume=1: on start, reconnect to the last app straight away\n"
            "auto_resume=%d\n"
            "# hud_corner: where the stats HUD sits, 0 top left, 1 top right, 2 bottom left, 3 bottom right\n"
            "hud_corner=%d\n"
            "# hud_size: the stats HUD's size, 0 small (70%%), 1 medium (80%%), 2 large (100%%)\n"
            "hud_size=%d\n"
            "# rumble=0: the PC's controller rumble isn't passed on to the pads\n"
            "rumble=%d\n"
            "# wake_on_lan=1: wake the PC with a magic packet to host_mac (the PC reports it) before connecting\n"
            "wake_on_lan=%d\n"
            "host_mac=%s\n"
            "# app_id: the host's id for app (for its cover); recent1-4: apps streamed lately, \"id|name\"\n"
            "app_id=%d\n"
            "# show_recent=0 hides the Play tab's Recent row\n"
            "show_recent=%d\n"
            "# menu_combo: held a second while streaming, opens the stream menu: 0 Back, 1 Start, 2 Back + Start,\n"
            "# 3 LB + RB\n"
            "menu_combo=%d\n"
            "# pointer_speed: mouse mode's pointer, 0 slow, 1 normal, 2 fast\n"
            "pointer_speed=%d\n"
            "# picture=1080: a 1920x1080 picture on a 1080p TV, not 1280x720 scaled up; read when the app starts.\n"
            "# If the screen stays black or scrambled with it, set picture=720 here.\n"
            "picture=%d\n",
            c->host, c->app, c->width, c->height, c->fps, c->bitrate_kbps, c->stats_mode,
            c->fast_decode ? 1 : 0, c->audio_packet_ms, c->audio_on_host ? 1 : 0, c->low_latency ? 1 : 0, c->decode ? 1 : 0, c->draw ? 1 : 0, c->packet_size, c->slices,
            c->codec == CODEC_MPEG2 ? "mpeg2" : "h264", c->logging ? 1 : 0, c->ui_sounds ? 1 : 0, c->theme, c->auto_resume ? 1 : 0, c->hud_corner, c->hud_size,
            c->rumble ? 1 : 0, c->wake_on_lan ? 1 : 0, c->host_mac, c->app_id,
            c->show_recent ? 1 : 0, c->menu_combo, c->pointer_speed, c->picture_1080p ? 1080 : 720);
    for (int i = 0; i < CONFIG_RECENT && c->recent_name[i][0]; i++)
        fprintf(f, "recent%d=%d|%s\n", i + 1, c->recent_id[i], c->recent_name[i]);
    fclose(f);
    return true;
}

void config_add_recent(Config *c, int id, const char *name)
{
    int keep_id[CONFIG_RECENT];
    char keep_name[CONFIG_RECENT][64];
    int n = 0;
    for (int i = 0; i < CONFIG_RECENT && c->recent_name[i][0] && n < CONFIG_RECENT - 1; i++)
        if (strcmp(c->recent_name[i], name) != 0) {
            keep_id[n] = c->recent_id[i];
            snprintf(keep_name[n], sizeof keep_name[n], "%s", c->recent_name[i]);
            n++;
        }
    c->recent_id[0] = id;
    snprintf(c->recent_name[0], sizeof c->recent_name[0], "%.63s", name);   /* longer names are cut */
    for (int i = 0; i < CONFIG_RECENT - 1; i++) {
        c->recent_id[i + 1] = i < n ? keep_id[i] : 0;
        if (i < n)
            snprintf(c->recent_name[i + 1], sizeof c->recent_name[i + 1], "%.63s", keep_name[i]);
        else
            c->recent_name[i + 1][0] = 0;
    }
}

void config_remove_recent(Config *c, int i)
{
    if (i < 0 || i >= CONFIG_RECENT)
        return;
    for (int k = i; k < CONFIG_RECENT - 1; k++) {
        c->recent_id[k] = c->recent_id[k + 1];
        memcpy(c->recent_name[k], c->recent_name[k + 1], sizeof c->recent_name[k]);
    }
    c->recent_id[CONFIG_RECENT - 1] = 0;
    c->recent_name[CONFIG_RECENT - 1][0] = 0;
}

uint16_t menu_combo_buttons(int combo)
{
    switch (combo) {
    case COMBO_START: return INPUT_GAMEPAD_START;
    case COMBO_BACK_START: return INPUT_GAMEPAD_BACK | INPUT_GAMEPAD_START;
    case COMBO_LB_RB: return INPUT_GAMEPAD_LEFT_SHOULDER | INPUT_GAMEPAD_RIGHT_SHOULDER;
    default: return INPUT_GAMEPAD_BACK;
    }
}

const char *menu_combo_name(int combo)
{
    static const char *const names[MENU_COMBOS] = { "Back", "Start", "Back + Start", "LB + RB" };
    return names[combo >= 0 && combo < MENU_COMBOS ? combo : COMBO_BACK];
}
