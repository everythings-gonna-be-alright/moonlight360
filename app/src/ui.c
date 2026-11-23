#include "ui.h"

#include "config.h"

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
    return ms >= 40 ? THEME_ERROR : ms >= 15 ? THEME_WARN : THEME_GOOD;
}

static uint32_t loss_colour(float pct)
{
    return pct >= 1.0f ? THEME_ERROR : pct > 0.0f ? THEME_WARN : THEME_GOOD;
}

static uint32_t fps_colour(const HudStats *h)
{
    if (h->target_fps <= 0.0f)
        return THEME_TEXT;
    const float share = h->fps / h->target_fps;
    return share >= 0.95f ? THEME_GOOD : share >= 0.8f ? THEME_WARN : THEME_ERROR;
}

static uint32_t latency_colour(float ms)
{
    return ms < 50.0f ? THEME_GOOD : ms < 80.0f ? THEME_WARN : THEME_ERROR;
}

/* The worst of three colours: red over orange over green. */
static uint32_t worst(uint32_t a, uint32_t b)
{
    if (a == THEME_ERROR || b == THEME_ERROR)
        return THEME_ERROR;
    return a == THEME_WARN || b == THEME_WARN ? THEME_WARN : a;
}

/* The total of the latency parts known; *partial if the PC's share isn't. -1 if nothing is measured yet. */
static float latency_total(const HudStats *h, bool *partial)
{
    *partial = h->host_ms < 0;
    if (!h->ready || h->display_ms < 0)
        return -1.0f;
    return (h->host_ms > 0 ? h->host_ms : 0.0f) + h->net_ms + h->decode_ms + h->display_ms;
}

enum {
    HUD_X = 12, HUD_Y = 10, HUD_PAD = 12, HUD_ROW = 24, HUD_W = 470,
    HUD_PANEL = 0xC0080C14,
    NET_GRAPH_W = NET_HISTORY * 2, NET_GRAPH_H = 18,
    LOAD_BAR_W = 6, LOAD_BAR_H = 16,
    LATENCY_SCALE_MS = 100,   /* the latency bar's full width */
};

/* The latency parts, in the bar and its legend. */
static const uint32_t part_colours[4] = { 0xFF9D7BFF, 0xFF4A9BFF, 0xFFE8B04A, 0xFF4CC3B0 };
static const char *const part_names[4] = { "PC", "net", "decode", "display" };

/* Text pieces drawn one after another; returns the x after the last. */
typedef struct {
    const char *text;
    uint32_t colour;
} Piece;

static int32_t pieces_width(const Piece *p, int n)
{
    int32_t w = 0;
    for (int i = 0; i < n; i++)
        w += gfx_text_width(p[i].text, 1);
    return w;
}

static int32_t draw_pieces(int32_t x, int32_t y, const Piece *p, int n)
{
    for (int i = 0; i < n; i++) {
        gfx_text(x, y, 1, p[i].colour, p[i].text);
        x += gfx_text_width(p[i].text, 1);
    }
    return x;
}

/* A bar per arriving frame, as tall as the time since the one before (up to three frame times), with a line at one
 * frame time. Green on time, orange more than two frames late, red where frames went missing before it. */
static void draw_arrivals(float gx, float gy, const NetStats *net)
{
    const float gw = (float)NET_GRAPH_W, gh = (float)NET_GRAPH_H;
    gfx_rect(gx - 3.0f, gy - 2.0f, gw + 6.0f, gh + 4.0f, 3.0f, 0x60000000);
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
        gfx_rect(gx + (float)((start + i) * 2), gy + gh - bh, 1.0f, bh, 0.0f, colour);
    }
    gfx_rect(gx - 1.0f, gy + gh - gh / 3.0f, gw + 1.0f, 1.0f, 0.0f, 0x60FFFFFF);   /* one frame time */
}

/* One line: health dot, frame rate, latency, ping, loss. */
static void draw_compact(const HudStats *h, const NetStats *net)
{
    char fps[24], lat[24], ping[24], loss[24];
    bool partial;
    const float total = latency_total(h, &partial);
    snprintf(fps, sizeof fps, h->ready ? "%.0f fps" : "-- fps", h->fps);
    if (total >= 0)
        snprintf(lat, sizeof lat, "%.0f%s ms", total, partial ? "+" : "");
    else
        snprintf(lat, sizeof lat, "-- ms");
    if (net->rtt_ms >= 0)
        snprintf(ping, sizeof ping, "ping %d ms", net->rtt_ms);
    else
        snprintf(ping, sizeof ping, "ping --");
    snprintf(loss, sizeof loss, net->loss_pct > 0 ? "loss %.1f%%" : "loss 0%%", net->loss_pct);
    const uint32_t sep = THEME_TEXT_3;
    const Piece p[] = {
        { fps, h->ready ? fps_colour(h) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { lat, total >= 0 ? latency_colour(total) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { ping, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { loss, loss_colour(net->loss_pct) },
    };
    const int n = (int)(sizeof p / sizeof p[0]);
    const float w = (float)(pieces_width(p, n) + 2 * HUD_PAD + 16), hgt = 32.0f;
    gfx_rect(HUD_X, HUD_Y, w, hgt, hgt / 2, HUD_PANEL);
    const uint32_t health = h->ready ? worst(worst(fps_colour(h), loss_colour(net->loss_pct)),
                                             total >= 0 ? latency_colour(total) : THEME_GOOD)
                                     : THEME_TEXT_3;
    gfx_circle(HUD_X + HUD_PAD + 4.0f, HUD_Y + hgt / 2, 4.0f, health);
    draw_pieces(HUD_X + HUD_PAD + 16, HUD_Y + 6, p, n);
}

/* Headline, latency bar and legend, network, load, and any trouble of the last five seconds. */
static void draw_detailed(const HudStats *h, const PerfReadings *perf, const NetStats *net)
{
    char text[128], a[32], b[32];
    const bool trouble = h->dropped || h->errors || h->audio_gaps;
    const int rows = 5 + (trouble ? 1 : 0);
    const float panel_h = (float)(2 * HUD_PAD + rows * HUD_ROW - 6);
    gfx_rect(HUD_X, HUD_Y, HUD_W + 2 * HUD_PAD, panel_h, 10.0f, HUD_PANEL);
    const int32_t x = HUD_X + HUD_PAD, right = x + HUD_W;
    int32_t y = HUD_Y + HUD_PAD;

    /* Headline: health, frame rate, latency; the stream's format on the right. */
    bool partial;
    const float total = latency_total(h, &partial);
    const uint32_t health = h->ready ? worst(worst(fps_colour(h), loss_colour(net->loss_pct)),
                                             total >= 0 ? latency_colour(total) : THEME_GOOD)
                                     : THEME_TEXT_3;
    gfx_circle((float)x + 5.0f, (float)y + 10.0f, 5.0f, health);
    snprintf(a, sizeof a, h->ready ? "%.0f" : "--", h->fps);
    if (total >= 0)
        snprintf(b, sizeof b, "%.0f%s", total, partial ? "+" : "");
    else
        snprintf(b, sizeof b, "--");
    const Piece head[] = {
        { a, h->ready ? fps_colour(h) : THEME_TEXT_3 }, { " fps    ", THEME_TEXT_2 },
        { b, total >= 0 ? latency_colour(total) : THEME_TEXT_3 }, { " ms latency", THEME_TEXT_2 },
    };
    draw_pieces(x + 18, y, head, 4);
    snprintf(text, sizeof text, "%dp%s%s%s", h->height, h->cabac < 0 ? "" : h->cabac ? "  CABAC" : "  CAVLC",
             h->fast_decode ? "  fast" : "", h->gpu_decode ? "  GPU" : "");
    gfx_text_right(right, y, 1, THEME_TEXT_3, text);
    y += HUD_ROW;

    /* Where the latency goes: a bar as long as the total (full width = 100 ms), split by part, and its legend. */
    const float parts[4] = { h->host_ms > 0 ? h->host_ms : 0.0f, h->net_ms, h->decode_ms,
                             h->display_ms > 0 ? h->display_ms : 0.0f };
    const float bar_w = (float)HUD_W;
    gfx_rect((float)x, (float)y + 2.0f, bar_w, 8.0f, 4.0f, THEME_TRACK);
    if (total > 0) {
        float bx = (float)x;
        for (int i = 0; i < 4; i++) {
            float w = bar_w * parts[i] / LATENCY_SCALE_MS;
            if (bx + w > x + bar_w)
                w = x + bar_w - bx;
            if (w >= 1.0f)
                gfx_rect(bx, (float)y + 2.0f, w, 8.0f, i == 0 || bx + w >= x + bar_w - 1 ? 4.0f : 0.0f,
                         part_colours[i]);
            bx += w;
        }
    }
    y += 16;
    int32_t lx = x;
    for (int i = 0; i < 4; i++) {
        gfx_rect((float)lx, (float)y + 6.0f, 8.0f, 8.0f, 2.0f, part_colours[i]);
        lx += 14;
        if (i == 0 && h->host_ms < 0)
            snprintf(text, sizeof text, "%s ?", part_names[i]);
        else if (!h->ready || (i == 3 && h->display_ms < 0))
            snprintf(text, sizeof text, "%s --", part_names[i]);
        else
            snprintf(text, sizeof text, "%s %.0f", part_names[i], parts[i]);
        gfx_text(lx, y, 1, THEME_TEXT_2, text);
        lx += gfx_text_width(text, 1) + 18;
    }
    gfx_text_right(right, y, 1, THEME_TEXT_3, "ms");
    y += HUD_ROW;

    /* Network: ping and loss, and the last frames' arrivals on the right. */
    if (net->rtt_ms >= 0)
        snprintf(a, sizeof a, "%d", net->rtt_ms);
    else
        snprintf(a, sizeof a, "--");
    snprintf(b, sizeof b, net->loss_pct > 0 ? "%.1f%%" : "0%%", net->loss_pct);
    const Piece netp[] = {
        { "Ping ", THEME_TEXT_2 }, { a, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3 },
        { " ms    Loss ", THEME_TEXT_2 }, { b, loss_colour(net->loss_pct) },
    };
    draw_pieces(x, y, netp, 4);   /* lost frames show in the graph, as red bars */
    draw_arrivals((float)(right - NET_GRAPH_W), (float)y + 1.0f, net);
    y += HUD_ROW;

    /* Load: a small bar per hardware thread, grouped by core; the GPU's; CPU and GPU temperatures. */
    snprintf(text, sizeof text, perf->cpu_average >= 0 ? "CPU %d%%" : "CPU --", perf->cpu_average);
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
    bx += 10.0f;
    snprintf(text, sizeof text, perf->gpu >= 0 ? "GPU %d%%" : "GPU --", perf->gpu);
    gfx_text((int32_t)bx, y, 1, THEME_TEXT_2, text);
    bx += (float)(gfx_text_width("GPU 100%", 1) + 8);
    const float gpu = perf->gpu < 0 ? 0.0f : (float)perf->gpu;
    gfx_rect(bx, (float)y + 6.0f, 56.0f, 8.0f, 4.0f, THEME_TRACK);
    if (gpu > 0)
        gfx_rect(bx, (float)y + 6.0f, 8.0f + 48.0f * gpu / 100.0f, 8.0f, 4.0f, load_colour(perf->gpu));
    if (perf->temp_cpu > 0) {
        const float hottest = perf->temp_cpu > perf->temp_gpu ? perf->temp_cpu : perf->temp_gpu;
        snprintf(text, sizeof text, "%.0f\xC2\xB0 / %.0f\xC2\xB0" "C", perf->temp_cpu, perf->temp_gpu);
        gfx_text_right(right, y, 1, temperature_colour(hottest) == THEME_TEXT_2 ? THEME_TEXT_3
                                                                              : temperature_colour(hottest), text);
    }
    y += HUD_ROW;

    /* Trouble, only when there is some. */
    if (trouble) {
        text[0] = 0;
        size_t n = 0;
        if (h->dropped)
            n += (size_t)snprintf(text + n, sizeof text - n, "%u dropped   ", (unsigned)h->dropped);
        if (h->errors && n < sizeof text)
            n += (size_t)snprintf(text + n, sizeof text - n, "%u decode errors   ", (unsigned)h->errors);
        if (h->audio_gaps && n < sizeof text)
            snprintf(text + n, sizeof text - n, "%u audio gaps", (unsigned)h->audio_gaps);
        gfx_text(x, y, 1, THEME_WARN, text);
        gfx_text_right(right, y, 1, THEME_TEXT_3, "last 5 s");
    }
}

void ui_draw_stats(int mode, const HudStats *hud, const PerfReadings *perf, const NetStats *net)
{
    if (mode == STATS_COMPACT)
        draw_compact(hud, net);
    else if (mode == STATS_DETAILED)
        draw_detailed(hud, perf, net);
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
