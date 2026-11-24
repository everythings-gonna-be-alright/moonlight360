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
/* Connecting: a card with the app (title) and where and how (subtitle), the steps across it (done with a tick, the one
 * under way glowing with a spinner, those to come hollow) and what the current step is doing (detail). names has count
 * entries; current is the one under way. */
void ui_draw_progress(uint32_t width, uint32_t height, const char *title, const char *subtitle,
                      const char *const *names, int count, int current, const char *detail);
/* Button hints along the bottom: buttons[i] (see gfx_button) with labels[i]; a NULL label shares the next one's. */
void ui_draw_hints(const char *buttons, const char *const *labels);
/* The stats HUD at the top left, over the video. STATS_COMPACT: one line (health, frame rate, latency, ping, loss).
 * STATS_DETAILED: a headline, then latency, network, CPU and GPU, each a value with a graph of its recent past, and
 * any trouble of the last five seconds. STATS_OFF draws nothing. */
/* corner: where its panel goes (HudCorner, config.h); size: how big (HudSize). */
void ui_draw_stats(int mode, int corner, int size, const HudStats *hud, const PerfReadings *perf, const NetStats *net,
                   const HudHistory *history);
/* Temperatures, small, at the bottom right of the menus. */
void ui_draw_temperatures(const PerfReadings *perf);
