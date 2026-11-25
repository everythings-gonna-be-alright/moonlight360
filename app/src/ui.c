#include "ui.h"

#include "config.h"

#include <math.h>
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

/* The title size (37) drawn at 75%: about 28 pixels, between the body text and the titles. */
void ui_draw_wordmark(int32_t x, int32_t cy)
{
    const float k = 0.75f;
    const int32_t y = cy - 20;
    gfx_scale_begin(k, (float)x, (float)cy);
    gfx_text(x, y, 2, THEME_TEXT, "Moonlight");
    gfx_text(x + gfx_text_width("Moonlight ", 2), y, 2, THEME_ACCENT, "360");
    gfx_scale_end();
}

bool ui_fresh(void)
{
    return theme_look == LOOK_FRESH;
}

void ui_draw_veil(uint32_t width, uint32_t height, float alpha)
{
    if (alpha <= 0.0f)
        return;
    const uint32_t a = alpha >= 1.0f ? 255u : (uint32_t)(alpha * 255.0f);
    gfx_rect(0.0f, 0.0f, (float)width, (float)height, 0.0f, a << 24 | (THEME_BG & 0x00FFFFFF));
}

void ui_draw_header(void)
{
    if (theme_look == LOOK_FRESH) {   /* Fresh: the name alone */
        ui_draw_wordmark(THEME_LEFT, THEME_HEADER_Y + 22);
        return;
    }
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
        gfx_spinner((float)cx, (float)y + 32.0f, 26.0f, THEME_ACCENT);
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
            gfx_rect((float)bx, (float)y, PIN_BOX_W, PIN_BOX_H, 16.0f, THEME_ACCENT_SOFT);
            gfx_rect((float)bx + 2.0f, (float)y + 2.0f, PIN_BOX_W - 4.0f, PIN_BOX_H - 4.0f, 14.0f, THEME_SURFACE);
            const char digit[2] = { pin[i], 0 };
            gfx_text_center(bx + PIN_BOX_W / 2, y + (PIN_BOX_H - 80) / 2, 4, THEME_TEXT, digit);
        }
        gfx_spinner((float)cx, (float)(y + PIN_BOX_H + 44), 12.0f, THEME_ACCENT);
    }

    static const char *const labels[] = { "Exit" };
    ui_draw_hints("K", labels);
}

void ui_draw_progress(uint32_t width, uint32_t height, const char *title, const char *subtitle,
                      const char *const *names, int count, int current, const char *detail)
{
    if (theme_look == LOOK_FRESH) {
        menu_draw_progress_fresh(width, height, title, subtitle, names, count, current, detail);
        return;
    }
    enum { CARD_W = 820, CARD_H = 380, NODE_R = 16, TRACK_H = 4 };
    ui_draw_background(width, height);
    ui_draw_header();
    const uint32_t frame = gfx_frame();
    const float t = (float)frame / 60.0f;

    /* Time on this connection: from the first frame drawn after a gap (a new connection). */
    static uint32_t last_frame, started;
    if (frame - last_frame > 2)
        started = frame;
    last_frame = frame;

    const float cx = (float)width / 2.0f, cy = (float)height / 2.0f;
    const float x = cx - CARD_W / 2.0f, y = cy - CARD_H / 2.0f - 6.0f;
    gfx_rect(x, y, CARD_W, CARD_H, THEME_RADIUS, THEME_BORDER);
    gfx_rect(x + 1.0f, y + 1.0f, CARD_W - 2.0f, CARD_H - 2.0f, THEME_RADIUS - 1, THEME_SURFACE);

    /* What's being connected: the app, big, and where and how. */
    const int32_t left = (int32_t)x + 44;
    gfx_text(left, (int32_t)y + 34, 1, THEME_ACCENT_TEXT, "CONNECTING");
    gfx_text(left, (int32_t)y + 58, 3, THEME_TEXT, title);
    gfx_text(left, (int32_t)y + 124, 1, THEME_TEXT_2, subtitle);
    char seconds[16];
    snprintf(seconds, sizeof seconds, "%u s", (unsigned)((frame - started) / 60));
    gfx_text_right((int32_t)(x + CARD_W) - 44, (int32_t)y + 34, 1, THEME_TEXT_3, seconds);

    /* The steps across the card: a track whose orange part slides on to the step under way. */
    const float track_y = y + 214.0f;
    const float first_x = x + 92.0f, span = CARD_W - 184.0f;
    const float step_w = count > 1 ? span / (float)(count - 1) : 0.0f;
    static float fill = -1.0f;
    const float target = step_w * (float)current;
    fill = fill < 0.0f || target < fill - 1.0f ? target : fill + (target - fill) * 0.12f;
    gfx_rect(first_x, track_y - TRACK_H / 2.0f, span, TRACK_H, TRACK_H / 2.0f, THEME_TRACK);
    if (fill > 0.0f)
        gfx_rect(first_x, track_y - TRACK_H / 2.0f, fill, TRACK_H, TRACK_H / 2.0f, THEME_GOOD);
    for (int i = 0; i < count; i++) {
        const float nx = first_x + step_w * (float)i;
        uint32_t label = THEME_TEXT_3;
        if (i < current) {
            gfx_circle(nx, track_y, NODE_R, THEME_GOOD);
            gfx_text_center((int32_t)nx, (int32_t)track_y - 10, 1, THEME_ON_ACCENT, "\xE2\x9C\x93");   /* U+2713 */
            label = THEME_TEXT_2;
        } else if (i == current) {
            /* A glow that breathes, the ring and a spinner in it. */
            const float breath = 0.5f + 0.5f * sinf(t * 6.2831853f / 1.6f);
            gfx_circle(nx, track_y, NODE_R + 6.0f + 4.0f * breath,
                       (uint32_t)(0x18 + 0x20 * breath) << 24 | (THEME_ACCENT & 0x00FFFFFF));
            gfx_circle(nx, track_y, NODE_R, THEME_ACCENT);
            gfx_circle(nx, track_y, NODE_R - 3.0f, THEME_SURFACE);
            gfx_spinner(nx, track_y, NODE_R - 8.0f, THEME_ACCENT);
            label = THEME_TEXT;
        } else {
            gfx_circle(nx, track_y, NODE_R, THEME_TRACK);
            gfx_circle(nx, track_y, NODE_R - 3.0f, THEME_SURFACE);
        }
        gfx_text_center((int32_t)nx, (int32_t)track_y + NODE_R + 14, 1, label, names[i]);
    }

    /* What the step under way is doing. */
    const float dy = y + CARD_H - 76.0f;
    gfx_rect(x + 32.0f, dy, CARD_W - 64.0f, 48.0f, 12.0f, THEME_ROW_SELECTED);
    char line[160];
    gfx_fit(line, sizeof line, detail && detail[0] ? detail : names[current < count ? current : count - 1], 70);
    gfx_circle(x + 58.0f, dy + 24.0f, 5.0f, THEME_ACCENT);
    gfx_text(left + 30, (int32_t)dy + 14, 1, THEME_TEXT_2, line);

    static const char *const labels[] = { "Exit" };
    ui_draw_hints("K", labels);
}

/* A small "Stand with Ukraine" badge at the right end of the footer: the flag and the words. */
static const char UKRAINE_TEXT[] = "Stand with Ukraine";
static const uint32_t UKRAINE_BLUE = 0xFF0057B7, UKRAINE_YELLOW = 0xFFFFD700;
enum { FLAG_W = 24, FLAG_H = 16, BADGE_GAP = 10 };

static int32_t badge_width(void)
{
    return FLAG_W + BADGE_GAP + gfx_text_width(UKRAINE_TEXT, 1);
}

static void draw_ukraine_badge(void)
{
    const float cy = (float)(THEME_FOOTER_Y + 14);
    const float x = (float)(THEME_RIGHT - badge_width());
    gfx_rect(x, cy - FLAG_H / 2, FLAG_W, FLAG_H / 2, 0.0f, UKRAINE_BLUE);
    gfx_rect(x, cy, FLAG_W, FLAG_H / 2, 0.0f, UKRAINE_YELLOW);
    gfx_text((int32_t)x + FLAG_W + BADGE_GAP, THEME_FOOTER_Y + 4, 1, THEME_TEXT_3, UKRAINE_TEXT);
}

void ui_draw_hints(const char *buttons, const char *const *labels)
{
    draw_ukraine_badge();
    gfx_rect(THEME_LEFT, THEME_FOOTER_Y - 18, THEME_RIGHT - THEME_LEFT, 1.0f, 0, THEME_DIVIDER);
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

/* Colours out of the enums below: past INT_MAX they would make an enum unsigned (theme.h). */
static const uint32_t HUD_PANEL = 0xC80C0C0E, LOAD_GRAPH = 0xB0FF7A1A;

enum {
    HUD_X = 12, HUD_Y = 10, HUD_PAD = 12, HUD_ROW = 24, HUD_W = 470,
    NET_GRAPH_W = NET_HISTORY * 2, NET_GRAPH_H = 22,
    LATENCY_SCALE_MS = 100,   /* the latency bar's full width */
};

/* The latency parts, in the bar and its legend. */
static const uint32_t part_colours[4] = { 0xFFB3B3B9, 0xFF4A9BFF, 0xFFFF7A1A, 0xFF4CC3B0 };
static const char *const part_names[4] = { "PC", "net", "dec", "disp" };

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
    gfx_rect(gx - 3.0f, gy - 2.0f, gw + 6.0f, gh + 4.0f, 3.0f, 0x60000000);   /* as graph_back */
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
        gfx_rect(gx + (float)((start + i) * 2), gy + gh - bh, 2.0f, bh, 0.0f, colour);
    }
    gfx_rect(gx - 1.0f, gy + gh - gh / 3.0f, gw + 1.0f, 1.0f, 0.0f, 0x60FFFFFF);   /* one frame time */
}

/* One line: health dot, frame rate, latency, ping, loss. */
typedef struct {
    char fps[24], lat[24], ping[24], loss[24];
} CompactText;

/* The compact line's pieces (into p, 7 of them, their text in t); returns its panel's width. */
static float compact_pieces(const HudStats *h, const NetStats *net, CompactText *t, Piece *p);

static void draw_compact(const HudStats *h, const NetStats *net)
{
    CompactText t;
    Piece p[7];
    const float w = compact_pieces(h, net, &t, p), hgt = 32.0f;
    const int n = 7;
    bool partial;
    const float total = latency_total(h, &partial);
    gfx_rect(HUD_X, HUD_Y, w, hgt, hgt / 2, HUD_PANEL);
    const uint32_t health = h->ready ? worst(worst(fps_colour(h), loss_colour(net->loss_pct)),
                                             total >= 0 ? latency_colour(total) : THEME_GOOD)
                                     : THEME_TEXT_3;
    gfx_circle(HUD_X + HUD_PAD + 4.0f, HUD_Y + hgt / 2, 4.0f, health);
    draw_pieces(HUD_X + HUD_PAD + 16, HUD_Y + 6, p, n);
}

static float compact_pieces(const HudStats *h, const NetStats *net, CompactText *t, Piece *out)
{
    char *fps = t->fps, *lat = t->lat, *ping = t->ping, *loss = t->loss;
    bool partial;
    const float total = latency_total(h, &partial);
    snprintf(fps, sizeof t->fps, h->ready ? "%.0f fps" : "-- fps", h->fps);
    if (total >= 0)
        snprintf(lat, sizeof t->lat, "%.0f%s ms", total, partial ? "+" : "");
    else
        snprintf(lat, sizeof t->lat, "-- ms");
    if (net->rtt_ms >= 0)
        snprintf(ping, sizeof t->ping, "ping %d ms", net->rtt_ms);
    else
        snprintf(ping, sizeof t->ping, "ping --");
    snprintf(loss, sizeof t->loss, net->loss_pct > 0 ? "loss %.1f%%" : "loss 0%%", net->loss_pct);
    const uint32_t sep = THEME_TEXT_3;
    const Piece p[] = {
        { fps, h->ready ? fps_colour(h) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { lat, total >= 0 ? latency_colour(total) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { ping, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3 }, { "  \xC2\xB7  ", sep },
        { loss, loss_colour(net->loss_pct) },
    };
    const int n = (int)(sizeof p / sizeof p[0]);
    for (int i = 0; i < n; i++)
        out[i] = p[i];
    return (float)(pieces_width(p, n) + 2 * HUD_PAD + 16);
}

/* The graphs' common frame, right-aligned on a row. */
enum { TEMP_X = 210, GRAPH_W = HUD_HISTORY * 2, GRAPH_H = 22, GRAPH_ROW = 30, LATENCY_GRAPH_H = 44, LATENCY_GRAPH_MS = 80 };

static void graph_back(float gx, float gy, float gh)
{
    gfx_rect(gx - 3.0f, gy - 2.0f, GRAPH_W + 6.0f, gh + 4.0f, 3.0f, 0x60000000);
}

/* Index of the i-th oldest entry of a ring with count entries and next. */
static int ring_at(int i, int count, int next)
{
    return count < HUD_HISTORY ? i : (next + i) % HUD_HISTORY;
}

/* Each shown frame's latency, its parts stacked in the legend's colours; full height is 80 ms (a frame over it is
 * cut off at the top). */
static void draw_latency_graph(float gx, float gy, const HudHistory *h)
{
    const float gh = (float)LATENCY_GRAPH_H;
    graph_back(gx, gy, gh);
    const int start = HUD_HISTORY - h->latency_count;   /* newest at the right edge */
    for (int i = 0; i < h->latency_count; i++) {
        const float *parts = h->latency[ring_at(i, h->latency_count, h->latency_next)];
        float y = gy + gh;
        for (int p = 0; p < LATENCY_PARTS && y > gy; p++) {
            float ph = gh * parts[p] / LATENCY_GRAPH_MS;
            ph = y - ph < gy ? y - gy : ph;
            if (ph > 0.0f)
                gfx_rect(gx + (float)((start + i) * 2), y - ph, 2.0f, ph, 0.0f, part_colours[p]);
            y -= ph;
        }
    }
    gfx_rect(gx, gy + gh / 2, GRAPH_W, 1.0f, 0.0f, 0x30FFFFFF);   /* 40 ms */
}

/* A load, 0-100, sample by sample; with peak (if not NULL) as a light mark on each. */
static void draw_load_graph(float gx, float gy, const uint8_t *load, const uint8_t *peak, int count, int next)
{
    graph_back(gx, gy, GRAPH_H);
    const int start = HUD_HISTORY - count;
    for (int i = 0; i < count; i++) {
        const int at = ring_at(i, count, next);
        const float x = gx + (float)((start + i) * 2);
        float bh = (float)GRAPH_H * load[at] / 100.0f;
        bh = bh < 1.0f ? 1.0f : bh;
        gfx_rect(x, gy + GRAPH_H - bh, 2.0f, bh, 0.0f, LOAD_GRAPH);
        if (peak && peak[at] > load[at])
            gfx_rect(x, gy + GRAPH_H - (float)GRAPH_H * peak[at] / 100.0f, 2.0f, 1.0f, 0.0f, 0xA0FFFFFF);
    }
}

/* Headline; then rows of a label and value on the left and a graph on the right: latency (with its parts' legend
 * under it), network, CPU, GPU; and any trouble of the last five seconds. */
/* The detailed panel's height: one more row while there's trouble to list. */
static float detailed_height(const HudStats *h)
{
    const bool trouble = h->dropped || h->errors || h->audio_gaps;
    return (float)(2 * HUD_PAD + HUD_ROW + 4 + (LATENCY_GRAPH_H + 10) + 3 * GRAPH_ROW + (trouble ? HUD_ROW : 0) - 6);
}

static void draw_detailed(const HudStats *h, const PerfReadings *perf, const NetStats *net, const HudHistory *hist)
{
    char text[128], a[32], b[32], c[32];
    const bool trouble = h->dropped || h->errors || h->audio_gaps;
    const float panel_h = detailed_height(h);
    gfx_rect(HUD_X, HUD_Y, HUD_W + 2 * HUD_PAD, panel_h, 10.0f, HUD_PANEL);
    const int32_t x = HUD_X + HUD_PAD, right = x + HUD_W;
    const float gx = (float)(right - GRAPH_W);
    int32_t y = HUD_Y + HUD_PAD;

    /* Headline: health and frame rate; the stream's format on the right. */
    bool partial;
    const float total = latency_total(h, &partial);
    const uint32_t health = h->ready ? worst(worst(fps_colour(h), loss_colour(net->loss_pct)),
                                             total >= 0 ? latency_colour(total) : THEME_GOOD)
                                     : THEME_TEXT_3;
    gfx_circle((float)x + 5.0f, (float)y + 10.0f, 5.0f, health);
    snprintf(a, sizeof a, h->ready ? "%.0f" : "--", h->fps);
    snprintf(b, sizeof b, " of %.0f fps", h->target_fps);
    const Piece head[] = { { a, h->ready ? fps_colour(h) : THEME_TEXT_3 }, { b, THEME_TEXT_2 } };
    draw_pieces(x + 18, y, head, 2);
    char quantiser[24] = "";
    if (h->qp >= 0)
        snprintf(quantiser, sizeof quantiser, "  QP %d", h->qp);
    snprintf(text, sizeof text, "%dp%s%s%s%s", h->height,
             h->mpeg2 ? "  MPEG-2" : h->cabac < 0 ? "" : h->cabac ? "  CABAC" : "  CAVLC",
             h->stream_kind == STREAM_LEAN ? " lean" : h->stream_kind == STREAM_NO_DEBLOCK ? " no-deblock" : "",
             h->fast_decode ? "  skip deblock" : "", quantiser);
    gfx_text_right(right, y, 1, THEME_TEXT_3, text);
    y += HUD_ROW + 4;

    /* Latency: the total and a graph of each frame's, then its parts' legend with the last second's averages. */
    if (total >= 0)
        snprintf(a, sizeof a, "%.0f%s", total, partial ? "+" : "");
    else
        snprintf(a, sizeof a, "--");
    const Piece lat[] = { { "Latency ", THEME_TEXT_2 }, { a, total >= 0 ? latency_colour(total) : THEME_TEXT_3 },
                          { " ms", THEME_TEXT_2 } };
    draw_pieces(x, y, lat, 3);
    draw_latency_graph(gx, (float)y, hist);
    y += 24;
    const float parts[4] = { h->host_ms > 0 ? h->host_ms : 0.0f, h->net_ms, h->decode_ms,
                             h->display_ms > 0 ? h->display_ms : 0.0f };
    /* The legend: each part's name and value in its colour in the graph. */
    int32_t lx = x;
    for (int i = 0; i < 4; i++) {
        if (i == 0 && h->host_ms < 0)
            snprintf(text, sizeof text, "%s ?", part_names[i]);
        else if (!h->ready || (i == 3 && h->display_ms < 0))
            snprintf(text, sizeof text, "%s --", part_names[i]);
        else
            snprintf(text, sizeof text, "%s %.0f", part_names[i], parts[i]);
        gfx_text(lx, y, 1, part_colours[i], text);
        lx += gfx_text_width(text, 1) + 14;
    }
    y += LATENCY_GRAPH_H + 10 - 24;

    /* Network: ping and loss; the last frames' arrivals. */
    if (net->rtt_ms >= 0)
        snprintf(a, sizeof a, "%d", net->rtt_ms);
    else
        snprintf(a, sizeof a, "--");
    snprintf(b, sizeof b, net->loss_pct > 0 ? "%.1f%%" : "0%%", net->loss_pct);
    const Piece netp[] = {
        { "Ping ", THEME_TEXT_2 }, { a, net->rtt_ms >= 0 ? ping_colour(net->rtt_ms) : THEME_TEXT_3 },
        { " ms   Loss ", THEME_TEXT_2 }, { b, loss_colour(net->loss_pct) },
    };
    draw_pieces(x, y, netp, 4);   /* lost frames show in the graph, as red bars */
    draw_arrivals(gx, (float)y + 1.0f, net);
    y += GRAPH_ROW;

    /* CPU: average and busiest hardware thread, temperature; load over time. */
    int peak = -1;
    for (int hw = 0; hw < 6; hw++)
        peak = perf->cpu[hw] > peak ? perf->cpu[hw] : peak;
    snprintf(a, sizeof a, perf->cpu_average >= 0 ? "%d%%" : "--", perf->cpu_average);
    snprintf(b, sizeof b, peak >= 0 ? "%d%%" : "--", peak);
    c[0] = 0;
    if (perf->temp_cpu > 0)
        snprintf(c, sizeof c, "%.0f\xC2\xB0" "C", perf->temp_cpu);
    /* One hardware thread near 100% is normal (the decode thread); colour the peak only when it is pinned. */
    const uint32_t peak_colour = peak >= 99 ? THEME_ERROR : peak >= 95 ? THEME_WARN : THEME_TEXT_2;
    const Piece cpu[] = {
        { "CPU ", THEME_TEXT_2 }, { a, perf->cpu_average >= 0 ? load_colour(perf->cpu_average) : THEME_TEXT_3 },
        { "  peak ", THEME_TEXT_3 }, { b, peak >= 0 ? peak_colour : THEME_TEXT_3 },
    };
    draw_pieces(x, y, cpu, 4);
    gfx_text(x + TEMP_X, y, 1,
             temperature_colour(perf->temp_cpu) == THEME_TEXT_2 ? THEME_TEXT_3 : temperature_colour(perf->temp_cpu), c);
    draw_load_graph(gx, (float)y + 1.0f, hist->cpu, hist->cpu_peak, hist->load_count, hist->load_next);
    y += GRAPH_ROW;

    /* GPU: load and temperature; load over time. */
    snprintf(a, sizeof a, perf->gpu >= 0 ? "%d%%" : "--", perf->gpu);
    c[0] = 0;
    if (perf->temp_gpu > 0)
        snprintf(c, sizeof c, "%.0f\xC2\xB0" "C", perf->temp_gpu);
    const Piece gpu[] = { { "GPU ", THEME_TEXT_2 }, { a, perf->gpu >= 0 ? load_colour(perf->gpu) : THEME_TEXT_3 } };
    draw_pieces(x, y, gpu, 2);
    gfx_text(x + TEMP_X, y, 1,
             temperature_colour(perf->temp_gpu) == THEME_TEXT_2 ? THEME_TEXT_3 : temperature_colour(perf->temp_gpu), c);
    draw_load_graph(gx, (float)y + 1.0f, hist->gpu, NULL, hist->load_count, hist->load_next);
    y += GRAPH_ROW;

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

void ui_draw_stats(int mode, int corner, int size, const HudStats *hud, const PerfReadings *perf, const NetStats *net,
                   const HudHistory *history)
{
    if (mode != STATS_COMPACT && mode != STATS_DETAILED)
        return;
    /* Its size (70, 80 or 100% of how it's laid out), its panel in the corner asked for, HUD_X and HUD_Y in from its
     * edges. */
    enum { SCREEN_W = 1280, SCREEN_H = 720 };
    const float scale = size == HUD_SMALL ? 0.7f : size == HUD_LARGE ? 1.0f : 0.8f;
    float w, hgt;
    if (mode == STATS_COMPACT) {
        CompactText t;
        Piece p[7];
        w = compact_pieces(hud, net, &t, p);
        hgt = 32.0f;
    } else {
        w = HUD_W + 2 * HUD_PAD;
        hgt = detailed_height(hud);
    }
    const bool right = corner == HUD_TOP_RIGHT || corner == HUD_BOTTOM_RIGHT;
    const bool bottom = corner == HUD_BOTTOM_LEFT || corner == HUD_BOTTOM_RIGHT;
    const float to_x = right ? SCREEN_W - HUD_X - w * scale : HUD_X;
    const float to_y = bottom ? SCREEN_H - HUD_Y - hgt * scale : HUD_Y;
    gfx_transform_begin(scale, HUD_X, HUD_Y, to_x, to_y);
    if (mode == STATS_COMPACT)
        draw_compact(hud, net);
    else if (mode == STATS_DETAILED)
        draw_detailed(hud, perf, net, history);
    gfx_scale_end();
}

void ui_draw_temperatures(const PerfReadings *perf)
{
    if (perf->temp_cpu <= 0)
        return;
    char text[64];
    snprintf(text, sizeof text, "CPU %.0f\xC2\xB0" "C   GPU %.0f\xC2\xB0" "C", perf->temp_cpu, perf->temp_gpu);
    const uint32_t c = perf->temp_cpu > perf->temp_gpu ? temperature_colour(perf->temp_cpu)
                                                        : temperature_colour(perf->temp_gpu);
    gfx_text_right(THEME_RIGHT - badge_width() - 32, THEME_FOOTER_Y + 4, 1, c == THEME_TEXT_2 ? THEME_TEXT_3 : c, text);
}
