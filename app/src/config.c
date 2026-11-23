#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    c->fps = 30;
    c->bitrate_kbps = 8000;
    c->show_stats = true;
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
    c->gpu_decode = false;
    c->net_priority = 2;
    c->decode_priority = 0;
    c->packet_size = 1392;

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
            c->show_stats = atoi(value) != 0;
        else if (!strcmp(key, "fast_decode"))
            c->fast_decode = atoi(value) != 0;
        else if (!strcmp(key, "decode"))
            c->decode = atoi(value) != 0;
        else if (!strcmp(key, "draw"))
            c->draw = atoi(value) != 0;
        else if (!strcmp(key, "gpu_decode"))
            c->gpu_decode = atoi(value) != 0;
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
            "# 1 shows decode/display rates at the top of the screen\n"
            "stats=%d\n"
            "# 1 skips H.264's deblocking filter: faster decoding, slightly blockier picture\n"
            "fast_decode=%d\n"
            "# audio_packet_ms: 20, 10 or 5. Keep 20 on Wi-Fi: 5 (Moonlight's usual) kills the console's Wi-Fi\n"
            "audio_packet_ms=%d\n"
            "# audio_on_host=1 plays sound on the PC (the host still sends the audio stream)\n"
            "audio_on_host=%d\n"
            "# gpu_decode=1: experimental GPU decoding (the GPU predicts each frame; no deblocking)\n"
            "gpu_decode=%d\n"
            "# test switches: decode=0 receives video without decoding it, draw=0 decodes but never draws\n"
            "# packet_size: 1392 or 1024 bytes. Keep 1392 on Wi-Fi: smaller packets mean more packets per second, and\n"
            "# thousands of small packets a second make the console's Wi-Fi stop receiving until reboot\n"
            "decode=%d\n"
            "draw=%d\n"
            "packet_size=%d\n",
            c->host, c->app, c->width, c->height, c->fps, c->bitrate_kbps, c->show_stats ? 1 : 0,
            c->fast_decode ? 1 : 0, c->audio_packet_ms, c->audio_on_host ? 1 : 0, c->gpu_decode ? 1 : 0, c->decode ? 1 : 0, c->draw ? 1 : 0, c->packet_size);
    fclose(f);
    return true;
}
