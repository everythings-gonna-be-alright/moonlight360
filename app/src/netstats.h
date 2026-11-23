/* What the stats HUD shows. NetStats: ping, lost frames and how the last frames arrived, filled by video.c (frames)
 * and main.c (ping). HudStats: the stream and its latency, filled by main.c once a second. Drawn by ui.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { NET_HISTORY = 90 };

typedef struct {
    int rtt_ms, rtt_var_ms;   /* moonlight-common-c's estimate over ENet; -1 before it has one */
    uint32_t lost_frames;     /* frames that never arrived whole, since the stream started */
    float loss_pct;           /* lost / (lost + arrived) over the last second */
    float budget_ms;          /* time between frames at the stream's frame rate */
    int count;                /* entries below, oldest first */
    float interval_ms[NET_HISTORY];   /* time since the frame before arrived */
    bool lost_before[NET_HISTORY];    /* frames were lost just before this one */
} NetStats;

typedef struct {
    bool ready;                 /* a second of the stream measured */
    float fps, target_fps;      /* frames shown a second, and the stream's rate */
    int width, height;
    int cabac;                  /* 1 CABAC, 0 CAVLC, -1 not known yet */
    bool fast_decode, gpu_decode;
    float gpu_ms, replay_ms;    /* GPU decoding's two parts, a frame */
    /* Latency in ms, last second's average: the PC's capture and encode (-1: the host doesn't say), network, decode,
     * display (-1: not measured yet). */
    float host_ms, net_ms, decode_ms, display_ms;
    /* Trouble in the last 5 seconds. */
    uint32_t dropped, errors, audio_gaps;
} HudStats;

/* The detailed HUD's graphs, newest last: a frame's latency in its four parts (PC, network, decode, display; ms),
 * shown frame by frame; and load (0-100: CPU average, its busiest hardware thread, GPU), twice a second. Rings: the
 * oldest entry is at next once count reaches HUD_HISTORY. */
enum { HUD_HISTORY = 90, LATENCY_PARTS = 4 };
typedef struct {
    float latency[HUD_HISTORY][LATENCY_PARTS];
    int latency_count, latency_next;
    uint8_t cpu[HUD_HISTORY], cpu_peak[HUD_HISTORY], gpu[HUD_HISTORY];
    int load_count, load_next;
} HudHistory;
