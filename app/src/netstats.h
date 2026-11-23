/* The network half of the stats panel: ping, lost frames, and how the last frames arrived. Filled by video.c (frames)
 * and main.c (ping), drawn by ui.c. */
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
