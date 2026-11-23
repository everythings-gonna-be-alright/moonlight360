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

/* Ping and loss on the left; on the right a bar per arriving frame, as tall as the time since the one before (up to
 * three frame times), with a line at one frame time. Green on time, orange more than two frames late, red where
 * frames went missing before it. */
enum { NET_GRAPH_H = 40, NET_BAR_W = 3, NET_BAR_STEP = 4, NET_LABEL_GAP = 20 };
static const char net_label_widest[] = "Ping 999 ms \xC2\xB1" "99   Loss 100.0% (99999)";

static int32_t net_row_width(void)
{
    return gfx_text_width(net_label_widest, 1) + NET_LABEL_GAP + NET_HISTORY * NET_BAR_STEP;
}

static void draw_net_row(int32_t x, int32_t y, const NetStats *net)
{
    char text[96];
    int32_t tx = x;
    if (net->rtt_ms >= 0)
        snprintf(text, sizeof text, "Ping %d ms \xC2\xB1%d", net->rtt_ms, net->rtt_var_ms);
    else
        snprintf(text, sizeof text, "Ping --");
    gfx_text(tx, y, 1, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3, text);
    tx += gfx_text_width(text, 1) + 24;
    snprintf(text, sizeof text, "Loss %.1f%% (%u)", net->loss_pct, (unsigned)net->lost_frames);
    gfx_text(tx, y, 1, loss_colour(net->loss_pct), text);
    gfx_text(x, y + 20, 1, THEME_TEXT_3, "Frame arrival");

    const float gx = (float)(x + gfx_text_width(net_label_widest, 1) + NET_LABEL_GAP), gy = (float)y;
    const float gw = (float)(NET_HISTORY * NET_BAR_STEP), gh = (float)NET_GRAPH_H;
    gfx_rect(gx - 4.0f, gy - 2.0f, gw + 6.0f, gh + 4.0f, 4.0f, 0x60000000);
    if (net->budget_ms <= 0.0f)
        return;
    const float top_ms = 3.0f * net->budget_ms;
    const int start = NET_HISTORY - net->count;   /* newest at the right edge */
    for (int i = 0; i < net->count; i++) {
        const float ms = net->interval_ms[i];
        float bh = gh * (ms < top_ms ? ms : top_ms) / top_ms;
        if (bh < 1.0f)
            bh = 1.0f;
        uint32_t colour = 0xB04CC38A;   /* THEME_GOOD, a little see-through */
        if (net->lost_before[i]) {
            colour = THEME_ERROR;
            bh = gh;
        } else if (ms > 2.0f * net->budget_ms) {
            colour = THEME_WARN;
        }
        gfx_rect(gx + (float)((start + i) * NET_BAR_STEP), gy + gh - bh, NET_BAR_W, bh, 0.0f, colour);
    }
    gfx_rect(gx - 2.0f, gy + gh - gh / 3.0f, gw + 2.0f, 1.0f, 0.0f, 0x80FFFFFF);   /* one frame time */
}

void ui_draw_stats(const char *line, const PerfReadings *perf, const NetStats *net)
{
    enum { X = 16, Y = 12, PAD = 14, BAR_W = 14, BAR_H = 26, BAR_GAP = 5 };
    const int32_t line_w = gfx_text_width(line, 1), net_w = net_row_width();
    const int32_t content_w = line_w > net_w ? line_w : net_w;
    const int32_t w = (content_w > 440 ? content_w : 440) + 2 * PAD;
    const int32_t h = 2 * PAD + 20 + 12 + BAR_H + 10 + 20 + 14 + NET_GRAPH_H;
    gfx_rect(X, Y, (float)w, (float)h, 12.0f, 0xC0080C14);
    int32_t x = X + PAD, y = Y + PAD;
    gfx_text(x, y, 1, THEME_TEXT, line);
    y += 20 + 12;

    /* CPU: a bar for each hardware thread, grouped by core. */
    char text[96];
    if (perf->cpu_average >= 0)
        snprintf(text, sizeof text, "CPU %3d%%", perf->cpu_average);
    else
        snprintf(text, sizeof text, "CPU   --");
    gfx_text(x, y + (BAR_H - 20) / 2, 1, THEME_TEXT_2, text);
    int32_t bx = x + gfx_text_width("CPU 100%", 1) + 12;
    for (int hw = 0; hw < 6; hw++) {
        const int load = perf->cpu[hw] < 0 ? 0 : perf->cpu[hw];
        gfx_rect((float)bx, (float)y, BAR_W, BAR_H, 3.0f, THEME_TRACK);
        const float fill = (float)BAR_H * (float)load / 100.0f;
        if (fill >= 1.0f)
            gfx_rect((float)bx, (float)y + BAR_H - fill, BAR_W, fill, 3.0f, load_colour(load));
        bx += BAR_W + (hw % 2 ? 3 * BAR_GAP : BAR_GAP);   /* wider gaps between cores */
    }

    /* GPU: one bar. */
    bx += 12;
    if (perf->gpu >= 0)
        snprintf(text, sizeof text, "GPU %3d%%", perf->gpu);
    else
        snprintf(text, sizeof text, "GPU   --");
    gfx_text(bx, y + (BAR_H - 20) / 2, 1, THEME_TEXT_2, text);
    bx += gfx_text_width("GPU 100%", 1) + 12;
    const float gpu_w = 120.0f, gpu = perf->gpu < 0 ? 0.0f : (float)perf->gpu;
    gfx_rect((float)bx, (float)y + BAR_H / 2 - 5, gpu_w, 10.0f, 5.0f, THEME_TRACK);
    if (gpu > 0)
        gfx_rect((float)bx, (float)y + BAR_H / 2 - 5, 10.0f + (gpu_w - 10.0f) * gpu / 100.0f, 10.0f, 5.0f,
                 load_colour(perf->gpu));
    y += BAR_H + 10;

    /* Temperatures. */
    if (perf->temp_cpu > 0) {
        struct { const char *name; float c; } t[] = {
            { "CPU", perf->temp_cpu }, { "GPU", perf->temp_gpu }, { "eDRAM", perf->temp_edram }, { "Board", perf->temp_board } };
        int32_t tx = x;
        for (int i = 0; i < 4; i++) {
            snprintf(text, sizeof text, "%s %.0f\xC2\xB0" "C", t[i].name, t[i].c);
            gfx_text(tx, y, 1, temperature_colour(t[i].c), text);
            tx += gfx_text_width(text, 1) + 24;
        }
    } else {
        gfx_text(x, y, 1, THEME_TEXT_3, "Temperatures: no reading");
    }
    draw_net_row(x, y + 20 + 14, net);
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
