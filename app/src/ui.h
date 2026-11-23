/* Screens shared by the menus: the background, the header, status screens and the stats line. Drawing thread only,
 * between gfx_frame_begin and the resolve. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "netstats.h"
#include "perf.h"

/* The full-screen backdrop. */
void ui_draw_background(uint32_t width, uint32_t height);
/* Logo and title at the top left. */
void ui_draw_header(void);
/* A card in the middle of the screen: a spinner if busy, a title, a body, and the PIN in big digits if pin is set. */
void ui_draw_status(uint32_t width, uint32_t height, const char *title, const char *body, const char *pin, bool busy);
/* Button hints along the bottom: buttons[i] (see gfx_button) with labels[i]; a NULL label shares the next one's. */
void ui_draw_hints(const char *buttons, const char *const *labels);
/* The stats HUD at the top left, over the video. STATS_COMPACT: one line (health, frame rate, latency, ping, loss).
 * STATS_DETAILED: a headline, where the latency goes, the network with a graph of the last frames' arrivals, the
 * console's load, and any trouble of the last five seconds. STATS_OFF draws nothing. */
void ui_draw_stats(int mode, const HudStats *hud, const PerfReadings *perf, const NetStats *net);
/* Temperatures, small, at the bottom right of the menus. */
void ui_draw_temperatures(const PerfReadings *perf);
