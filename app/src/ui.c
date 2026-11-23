#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "theme.h"

enum {
    BODY_CHARS = 72,
    PIN_BOX_W = 76,
    PIN_BOX_H = 104,
    PIN_GAP = 18,
};

/* Nothing to draw: the resolve clears every frame to THEME_BG (main.c CLEAR_ARGB). Not even a flat rectangle, as
 * the shapes' shader is what the console showed rhombs in (2026-11-23). */
void ui_draw_background(uint32_t width, uint32_t height)
{
    (void)width, (void)height;
}

void ui_draw_header(void)
{
    gfx_logo(THEME_LEFT, THEME_HEADER_Y, 48.0f);
    gfx_text(THEME_LEFT + 64, THEME_HEADER_Y + 4, 2, THEME_TEXT, "Moonlight");
    gfx_text(THEME_LEFT + 64 + gfx_text_width("Moonlight ", 2), THEME_HEADER_Y + 4, 2, THEME_ACCENT_TEXT, "360");
}

/* Lines of text, each centred on center_x. */
static void draw_centered_lines(int32_t center_x, int32_t y, uint32_t scale, uint32_t argb, char *text)
{
    for (char *line = text; line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        gfx_text_center(center_x, y, scale, argb, line);
        y += 20 * (int32_t)scale;
        line = next;
    }
}

void ui_draw_status(uint32_t width, uint32_t height, const char *title, const char *body, const char *pin, bool busy)
{
    ui_draw_background(width, height);
    ui_draw_header();

    char wrapped[1024];
    const int lines = gfx_wrap(wrapped, sizeof wrapped, body ? body : "", BODY_CHARS);
    const bool has_pin = pin && pin[0];
    const int32_t cx = (int32_t)width / 2;
    /* The column's height, centred: spinner, title, body, PIN. */
    int32_t column = 40 + 16 + lines * 20;
    if (busy && !has_pin)
        column += 64 + 28;
    if (has_pin)
        column += 40 + PIN_BOX_H;
    int32_t y = ((int32_t)height - column) / 2;

    if (busy && !has_pin) {
        gfx_spinner((float)cx, (float)y + 32.0f, 26.0f, THEME_ACCENT_TEXT);
        y += 64 + 28;
    }
    gfx_text_center(cx, y, 2, THEME_TEXT, title);
    y += 40 + 16;
    draw_centered_lines(cx, y, 1, THEME_TEXT_2, wrapped);
    y += lines * 20 + 40;

    if (has_pin) {
        const int32_t digits = (int32_t)strlen(pin);
        const int32_t row_w = digits * PIN_BOX_W + (digits - 1) * PIN_GAP;
        int32_t bx = cx - row_w / 2;
        for (int32_t i = 0; i < digits; i++, bx += PIN_BOX_W + PIN_GAP) {
            gfx_rect((float)bx, (float)y, PIN_BOX_W, PIN_BOX_H, 16.0f, THEME_BORDER);
            gfx_rect((float)bx + 1.0f, (float)y + 1.0f, PIN_BOX_W - 2.0f, PIN_BOX_H - 2.0f, 15.0f, THEME_SURFACE);
            const char digit[2] = { pin[i], 0 };
            gfx_text_center(bx + PIN_BOX_W / 2, y + (PIN_BOX_H - 80) / 2, 4, THEME_TEXT, digit);
        }
        gfx_spinner((float)cx, (float)(y + PIN_BOX_H + 44), 12.0f, THEME_TEXT_3);
    }

    static const char *const labels[] = { "Exit" };
    ui_draw_hints("K", labels);
}

void ui_draw_hints(const char *buttons, const char *const *labels)
{
    int32_t x = THEME_LEFT;
    for (int i = 0; buttons[i]; i++) {
        if (labels[i])
            x += gfx_hint(x, THEME_FOOTER_Y + 14, buttons[i], labels[i], THEME_TEXT_2);
        else   /* a button that shares the next one's label, as LB with RB */
            x += gfx_button(x, THEME_FOOTER_Y + 14, buttons[i]) + 6;
    }
}

static uint32_t load_colour(int percent)
{
    return percent >= 90 ? THEME_ERROR : percent >= 70 ? THEME_WARN : THEME_GOOD;
}

static uint32_t temperature_colour(float c)
{
    return c >= 85.0f ? THEME_ERROR : c >= 72.0f ? THEME_WARN : THEME_TEXT_2;
}

static uint32_t ping_colour(int ms)
{
    return ms >= 40 ? THEME_ERROR : ms >= 15 ? THEME_WARN : THEME_TEXT_2;
}

static uint32_t loss_colour(float pct)
{
    return pct >= 1.0f ? THEME_ERROR : pct > 0.0f ? THEME_WARN : THEME_TEXT_2;
}

enum {
    STATS_X = 12, STATS_Y = 10, STATS_PAD = 10, STATS_ROW = 22,
    NET_GRAPH_H = 18, NET_BAR_STEP = 2,   /* a 1-pixel bar every 2 pixels: NET_HISTORY frames in 180 */
    LOAD_BAR_W = 6, LOAD_BAR_H = 16,
};
/* The network line's widest text, so the graph after it never moves. */
static const char net_text_widest[] = "Ping 999\xC2\xB1" "99 ms  Loss 100.0% (9999)";

/* A bar per arriving frame, as tall as the time since the one before (up to three frame times), with a line at one
 * frame time. Green on time, orange more than two frames late, red where frames went missing before it. */
static void draw_arrivals(float gx, float gy, const NetStats *net)
{
    const float gw = (float)(NET_HISTORY * NET_BAR_STEP), gh = (float)NET_GRAPH_H;
    gfx_rect(gx - 3.0f, gy - 1.0f, gw + 5.0f, gh + 2.0f, 3.0f, 0x50000000);
    if (net->budget_ms <= 0.0f)
        return;
    const float top_ms = 3.0f * net->budget_ms;
    const int start = NET_HISTORY - net->count;   /* newest at the right edge */
    for (int i = 0; i < net->count; i++) {
        const float ms = net->interval_ms[i];
        float bh = gh * (ms < top_ms ? ms : top_ms) / top_ms;
        bh = bh < 1.0f ? 1.0f : bh;
        uint32_t colour = 0xB04CC38A;   /* THEME_GOOD, a little see-through */
        if (net->lost_before[i]) {
            colour = THEME_ERROR;
            bh = gh;
        } else if (ms > 2.0f * net->budget_ms) {
            colour = THEME_WARN;
        }
        gfx_rect(gx + (float)((start + i) * NET_BAR_STEP), gy + gh - bh, 1.0f, bh, 0.0f, colour);
    }
    gfx_rect(gx - 1.0f, gy + gh - gh / 3.0f, gw + 1.0f, 1.0f, 0.0f, 0x70FFFFFF);   /* one frame time */
}

void ui_draw_stats(const char *line, const char *latency, const PerfReadings *perf, const NetStats *net)
{
    char text[96];
    const int32_t net_w = gfx_text_width(net_text_widest, 1) + 14 + NET_HISTORY * NET_BAR_STEP;
    const int32_t load_w = gfx_text_width("CPU 100%", 1) + 8 + 3 * (2 * (LOAD_BAR_W + 2) + 6) + 6 +
                           gfx_text_width("GPU 100%", 1) + 8 + 48 + 16 + gfx_text_width("99\xC2\xB0/99\xC2\xB0" "C", 1);
    int32_t content_w = gfx_text_width(line, 1);
    const int32_t widths[] = { gfx_text_width(latency, 1), net_w, load_w };
    for (int i = 0; i < 3; i++)
        content_w = widths[i] > content_w ? widths[i] : content_w;
    gfx_rect(STATS_X, STATS_Y, (float)(content_w + 2 * STATS_PAD), (float)(4 * STATS_ROW + 2 * STATS_PAD - 4), 8.0f,
             0xB0080C14);
    const int32_t x = STATS_X + STATS_PAD;
    int32_t y = STATS_Y + STATS_PAD;

    /* The stream, then where the time goes. */
    gfx_text(x, y, 1, THEME_TEXT, line);
    y += STATS_ROW;
    gfx_text(x, y, 1, THEME_TEXT, latency);
    y += STATS_ROW;

    /* Network: ping, loss, and how the last frames arrived. */
    int32_t tx = x;
    if (net->rtt_ms >= 0)
        snprintf(text, sizeof text, "Ping %d\xC2\xB1%d ms", net->rtt_ms, net->rtt_var_ms);
    else
        snprintf(text, sizeof text, "Ping --");
    gfx_text(tx, y, 1, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3, text);
    tx += gfx_text_width(text, 1) + 16;
    snprintf(text, sizeof text, "Loss %.1f%% (%u)", net->loss_pct, (unsigned)net->lost_frames);
    gfx_text(tx, y, 1, loss_colour(net->loss_pct), text);
    draw_arrivals((float)(x + gfx_text_width(net_text_widest, 1) + 14), (float)y + 1.0f, net);
    y += STATS_ROW;

    /* Load: a small bar per hardware thread, grouped by core, the GPU's, then CPU/GPU temperatures. */
    if (perf->cpu_average >= 0)
        snprintf(text, sizeof text, "CPU %d%%", perf->cpu_average);
    else
        snprintf(text, sizeof text, "CPU --");
    gfx_text(x, y, 1, THEME_TEXT_2, text);
    float bx = (float)(x + gfx_text_width("CPU 100%", 1) + 8);
    for (int hw = 0; hw < 6; hw++) {
        const int load = perf->cpu[hw] < 0 ? 0 : perf->cpu[hw];
        gfx_rect(bx, (float)y + 2.0f, LOAD_BAR_W, LOAD_BAR_H, 2.0f, THEME_TRACK);
        const float fill = (float)LOAD_BAR_H * (float)load / 100.0f;
        if (fill >= 1.0f)
            gfx_rect(bx, (float)y + 2.0f + LOAD_BAR_H - fill, LOAD_BAR_W, fill, 2.0f, load_colour(load));
        bx += LOAD_BAR_W + (hw % 2 ? 8.0f : 2.0f);   /* wider gaps between cores */
    }
    bx += 6.0f;
    if (perf->gpu >= 0)
        snprintf(text, sizeof text, "GPU %d%%", perf->gpu);
    else
        snprintf(text, sizeof text, "GPU --");
    gfx_text((int32_t)bx, y, 1, THEME_TEXT_2, text);
    bx += (float)(gfx_text_width("GPU 100%", 1) + 8);
    const float gpu = perf->gpu < 0 ? 0.0f : (float)perf->gpu;
    gfx_rect(bx, (float)y + 6.0f, 48.0f, 8.0f, 4.0f, THEME_TRACK);
    if (gpu > 0)
        gfx_rect(bx, (float)y + 6.0f, 8.0f + 40.0f * gpu / 100.0f, 8.0f, 4.0f, load_colour(perf->gpu));
    bx += 48.0f + 16.0f;
    if (perf->temp_cpu > 0) {
        const float hottest = perf->temp_cpu > perf->temp_gpu ? perf->temp_cpu : perf->temp_gpu;
        snprintf(text, sizeof text, "%.0f\xC2\xB0/%.0f\xC2\xB0" "C", perf->temp_cpu, perf->temp_gpu);
        gfx_text((int32_t)bx, y, 1, temperature_colour(hottest), text);
    }
}

void ui_draw_temperatures(const PerfReadings *perf)
{
    if (perf->temp_cpu <= 0)
        return;
    char text[64];
    snprintf(text, sizeof text, "CPU %.0f\xC2\xB0" "C   GPU %.0f\xC2\xB0" "C", perf->temp_cpu, perf->temp_gpu);
    const uint32_t c = perf->temp_cpu > perf->temp_gpu ? temperature_colour(perf->temp_cpu)
                                                        : temperature_colour(perf->temp_gpu);
    gfx_text_right(THEME_RIGHT, THEME_FOOTER_Y + 4, 1, c == THEME_TEXT_2 ? THEME_TEXT_3 : c, text);
}
