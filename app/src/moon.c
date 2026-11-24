#include "moon.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "theme.h"

#define PI 3.14159265f
#define TAU 6.2831853f
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const uint32_t MOON = 0xFFF4EBCF, CRATER = 0xFFE2D5B0, STAR_RGB = 0x00E6E6EA, SATELLITE_RGB = 0x009A9AA2,
                      CLOUD_RGB = 0x0026262B, CLOUD_RIM_RGB = 0x003E3D40, BLOOD_MOON = 0xFFD9653B,
                      BLOOD_CRATER = 0xFFB04A2A, COMET_RGB = 0x00BFE4FF, BIRD = 0xFF0C0C0E, UFO_BODY = 0xFF9A9AA6,
                      UFO_DOME = 0xFFCFE8F0, AURORA_GREEN = 0x0057F2A6, AURORA_VIOLET = 0x009B6BFF,
                      FIREFLY_RGB = 0x00D9F56A, PLANE_RED = 0x00FF4A3A, PLANE_GREEN = 0x0046F27A;
static const float FRAMES_PER_SECOND = 60.0f;

/* Stars: x, y (in moon radii from the centre), size, twinkle period in seconds. */
static const float stars[][4] = { { -1.75f, -0.95f, 2.5f, 4.0f }, { 1.65f, -1.15f, 2.0f, 5.5f },
                                  { 1.80f, 0.85f, 2.5f, 4.7f },  { -1.55f, 1.15f, 2.0f, 6.1f },
                                  { 0.35f, -1.70f, 1.5f, 3.6f }, { -2.10f, 0.20f, 1.5f, 5.0f },
                                  { 2.25f, -0.25f, 1.5f, 4.3f }, { 0.95f, 1.60f, 1.5f, 5.8f } };

/* A random event: runs from start for length seconds; the next one may start at next. */
typedef struct {
    float start, length, next;
    float a, b, c, d;   /* its own random choices */
} Event;

/* First ones soon after the menu opens, so the sky shows it's alive. */
static Event shooting = { .next = 3.0f }, cloud = { .next = 9.0f }, glint = { .next = 1.5f }, satellite = { .next = 24.0f };
/* The rare ones, minutes apart, for whoever lingers: birds across the moon, a comet, a meteor shower, a blood moon,
 * and now and then a UFO. */
static Event birds = { .next = 30.0f }, comet = { .next = 45.0f }, shower = { .next = 70.0f },
             blood = { .next = 150.0f }, ufo = { .next = 240.0f };
/* And rarer still: fireflies, a constellation drawing itself, a plane, an aurora. */
static Event fireflies = { .next = 100.0f }, constellation = { .next = 120.0f }, plane = { .next = 190.0f },
             aurora = { .next = 300.0f };

static uint32_t seed = 0x9E3779B9u;

/* The sky's own clock, in seconds: it runs only while the sky is drawn, so after a stream or a while on another tab
 * the events carry on where they were, not all due at once. */
static float sky_seconds;
static uint32_t last_frame;
static bool drawn;

void moon_skip(float seconds)
{
    sky_seconds += seconds;
}

/* xorshift32: lo to hi. */
static float random_range(float lo, float hi)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return lo + (hi - lo) * (float)(seed >> 8) * (1.0f / 16777216.0f);
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static uint32_t with_alpha(uint32_t rgb, float alpha)
{
    return ((uint32_t)(clamp01(alpha) * 255.0f + 0.5f) << 24) | (rgb & 0x00FFFFFF);
}

/* rgb a share of the way from the background to colour, opaque: a shape that fades in without its overlaps showing. */
static uint32_t toward(uint32_t colour, float share)
{
    uint32_t out = 0xFF000000;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float from = (float)((THEME_BG >> shift) & 0xFF), to = (float)((colour >> shift) & 0xFF);
        out |= (uint32_t)(from + (to - from) * clamp01(share) + 0.5f) << shift;
    }
    return out;
}

/* Opaque colour a share of the way from a to b. */
static uint32_t mix(uint32_t a, uint32_t b, float share)
{
    uint32_t out = 0xFF000000;
    for (int shift = 0; shift <= 16; shift += 8) {
        const float from = (float)((a >> shift) & 0xFF), to = (float)((b >> shift) & 0xFF);
        out |= (uint32_t)(from + (to - from) * clamp01(share) + 0.5f) << shift;
    }
    return out;
}

/* Progress 0 to 1 through a running event, or -1 if it isn't running; starts it when it's due. */
static float progress(Event *e, float t, float length, float gap_min, float gap_max, bool *started)
{
    *started = false;
    if (e->length > 0.0f && t < e->start + e->length)
        return (t - e->start) / e->length;
    if (t < e->next)
        return -1.0f;
    e->start = t;
    e->length = length;
    e->next = t + length + random_range(gap_min, gap_max);
    *started = true;
    return 0.0f;
}

/* 0 at both ends, 1 in the middle, easing in over the first and out over the last edge share. */
static float fade(float p, float edge)
{
    return clamp01(p / edge) * clamp01((1.0f - p) / edge);
}

static void draw_stars(float cx, float cy, float r, float t)
{
    /* A glint: now and then one star flares into a four-point sparkle. */
    bool started;
    const float gp = progress(&glint, t, 1.6f, 2.5f, 6.0f, &started);
    if (started)
        glint.a = (float)(int)random_range(0.0f, (float)COUNT(stars) - 0.01f);
    const int flaring = gp >= 0.0f ? (int)glint.a : -1;

    for (int i = 0; i < COUNT(stars); i++) {
        const float x = cx + stars[i][0] * r, y = cy + stars[i][1] * r;
        const float shine = 0.5f + 0.5f * sinf(t * TAU / stars[i][3] + (float)i * 1.7f);
        float size = stars[i][2], alpha = 0.2f + 0.55f * shine;
        if (i == flaring) {
            const float e = sinf(gp * PI);
            const float arm = 3.0f + 11.0f * e;
            alpha += (1.0f - alpha) * e;
            size += 0.8f * e;
            gfx_rect(x - arm, y - 0.5f, 2.0f * arm, 1.0f, 0.5f, with_alpha(STAR_RGB, 0.8f * e));
            gfx_rect(x - 0.5f, y - arm, 1.0f, 2.0f * arm, 0.5f, with_alpha(STAR_RGB, 0.8f * e));
        }
        gfx_circle(x, y, size, with_alpha(STAR_RGB, alpha));
    }
}

/* A streak across the sky: a bright head from (x0, y0) along (dx, dy), slowing as it burns out, with a fading tail of
 * tail dots behind it. p from 0 to 1 over its flight. */
static void draw_streak(float x0, float y0, float dx, float dy, float p, int tail)
{
    const float eased = 1.0f - (1.0f - p) * (1.0f - p);
    const float hx = x0 + dx * eased, hy = y0 + dy * eased;
    const float bright = fade(p, 0.25f);
    for (int k = tail - 1; k >= 0; k--) {   /* back to front, the head last */
        const float back = (float)k / (float)tail;
        const float x = hx - dx * 0.30f * back, y = hy - dy * 0.30f * back;
        const float left = 1.0f - back;
        gfx_circle(x, y, 0.7f + 1.8f * left, with_alpha(STAR_RGB, bright * left * left));
    }
    gfx_circle(hx, hy, 5.0f, with_alpha(STAR_RGB, 0.22f * bright));   /* the head's glow */
    gfx_circle(hx, hy, 2.6f, with_alpha(0x00FFFFFF, bright));
}

/* A shooting star: a bright head with a fading tail, down and to the left, passing behind the moon. */
static void draw_shooting_star(float cx, float cy, float r, float t)
{
    bool started;
    const float p = progress(&shooting, t, 1.1f, 5.0f, 14.0f, &started);
    if (started) {
        shooting.a = cx + random_range(-0.6f, 2.0f) * r;   /* start */
        shooting.b = cy + random_range(-1.9f, -1.1f) * r;
        const float speed = random_range(0.8f, 1.2f);
        shooting.c = -2.6f * r * speed;   /* the whole flight */
        shooting.d = random_range(1.2f, 1.8f) * r * speed;
    }
    if (p >= 0.0f)
        draw_streak(shooting.a, shooting.b, shooting.c, shooting.d, p, 36);
}

/* A meteor shower: a handful of shooting stars from one point of the sky, one after another. */
static void draw_shower(float cx, float cy, float r, float t)
{
    enum { METEORS = 5 };
    bool started;
    const float p = progress(&shower, t, 4.0f, 120.0f, 240.0f, &started);
    if (started) {
        shower.a = cx + random_range(0.4f, 2.0f) * r;   /* the radiant: where they seem to come from */
        shower.b = cy + random_range(-2.0f, -1.5f) * r;
    }
    if (p < 0.0f)
        return;
    const float now = p * 4.0f;
    for (int i = 0; i < METEORS; i++) {
        /* Each its own start, place and slant, the same for every frame of the shower (from its index). */
        const float begin = 0.55f * (float)i + 0.12f * sinf((float)i * 2.3f);
        const float mp = (now - begin) / 0.9f;
        if (mp < 0.0f || mp > 1.0f)
            continue;
        const float spread = sinf((float)i * 1.7f + shower.a), slant = 0.9f + 0.4f * cosf((float)i * 2.9f);
        const float x0 = shower.a + spread * 0.5f * r, y0 = shower.b + 0.15f * r * cosf((float)i * 4.1f);
        draw_streak(x0, y0, -2.0f * r * slant, 1.3f * r * slant, mp, 24);
    }
}

/* A comet: slow, its long tail glowing behind it, across the top of the sky. */
static void draw_comet(float cx, float cy, float r, float t)
{
    enum { TAIL = 54 };
    bool started;
    const float p = progress(&comet, t, 9.0f, 90.0f, 180.0f, &started);
    if (started) {
        comet.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;   /* which way */
        comet.b = cy + random_range(-1.95f, -1.55f) * r;
    }
    if (p < 0.0f)
        return;
    const float seen = fade(p, 0.18f);
    const float hx = cx + comet.a * (p * 2.0f - 1.0f) * 2.2f * r, hy = comet.b + 0.25f * r * p;
    /* The tail streams back and a little up, widening and fading. */
    for (int k = TAIL - 1; k >= 0; k--) {
        const float back = (float)k / TAIL, left = 1.0f - back;
        const float x = hx - comet.a * back * 1.1f * r, y = hy - back * 0.28f * r - back * back * 0.12f * r;
        gfx_circle(x, y, 1.2f + 6.0f * back, with_alpha(COMET_RGB, seen * 0.16f * left * left));
    }
    gfx_circle(hx, hy, 7.0f, with_alpha(COMET_RGB, 0.25f * seen));
    gfx_circle(hx, hy, 2.4f, with_alpha(0x00FFFFFF, seen));
}

/* A UFO, rarely: it slides in, hovers beside the moon with its lights running, then darts away. */
static void draw_ufo(float cx, float cy, float r, float t)
{
    bool started;
    const float p = progress(&ufo, t, 6.0f, 300.0f, 480.0f, &started);
    if (started)
        ufo.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;   /* which side it hovers on */
    if (p < 0.0f)
        return;
    /* In over the first quarter, easing to a stop; hover; gone in the last tenth, fast. */
    const float hx = cx + ufo.a * 1.55f * r, hy = cy - 1.05f * r;
    float x = hx, y = hy + 3.0f * sinf(t * TAU / 1.4f);
    if (p < 0.25f) {
        const float q = 1.0f - p / 0.25f;
        x += ufo.a * q * q * 2.0f * r;
        y -= q * q * 0.8f * r;
    } else if (p > 0.9f) {
        const float q = (p - 0.9f) / 0.1f;
        x -= ufo.a * q * q * 3.0f * r;
        y -= q * q * 1.2f * r;
    }
    const float w = 0.26f * r;
    gfx_circle(x, y - 0.08f * r, 0.085f * r, UFO_DOME);
    gfx_rect(x - w / 2, y - 0.035f * r, w, 0.07f * r, 0.035f * r, UFO_BODY);
    static const uint32_t lights[3] = { 0x00FF7A1A, 0x0080FF9A, 0x00FFFFFF };
    for (int i = 0; i < 3; i++) {
        const bool on = ((int)(t * 6.0f) + i) % 3 == 0;
        gfx_circle(x + (float)(i - 1) * 0.08f * r, y, 2.2f, with_alpha(lights[i], on ? 1.0f : 0.35f));
    }
}

/* Birds: a little flock flapping across, in front of the moon; dark, so they show against it and not the sky. */
static void draw_birds(float cx, float cy, float r, float t)
{
    enum { FLOCK = 4, ARM = 11 };   /* dots a wing: enough to read as one stroke */
    bool started;
    const float p = progress(&birds, t, 7.0f, 60.0f, 120.0f, &started);
    if (started) {
        birds.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;   /* which way */
        birds.b = cy + random_range(-0.35f, 0.25f) * r;                /* across the moon */
    }
    if (p < 0.0f)
        return;
    for (int i = 0; i < FLOCK; i++) {
        /* A loose V: each a little behind and to the side of the one before. */
        const float lag = 0.13f * (float)i, side = (i % 2 ? 1.0f : -1.0f) * 0.07f * (float)((i + 1) / 2) * r;
        const float x = cx + birds.a * ((p - lag) * 2.0f - 1.0f) * 2.3f * r, y = birds.b + side;
        const float size = 5.2f - 0.5f * (float)i;
        const float flap = sinf(t * TAU * 2.2f + (float)i * 1.3f);   /* -1 wings down, 1 up */
        for (int k = 1; k <= ARM; k++) {   /* each wing: dots out from the body, bending with the flap */
            const float along = (float)k / ARM;
            const float dx = along * 3.0f * size, dy = -along * flap * 1.6f * size + along * along * size;
            const float thick = 1.7f - 0.6f * along;   /* wings thinning to their tips */
            gfx_circle(x - dx, y + dy, thick, BIRD);
            gfx_circle(x + dx, y + dy, thick, BIRD);
        }
        gfx_circle(x, y + 0.5f, 2.2f, BIRD);
    }
}

/* A satellite: a dim dot crawling across the sky with an orange light that blinks. */
static void draw_satellite(float cx, float cy, float r, float t)
{
    bool started;
    const float p = progress(&satellite, t, 28.0f, 30.0f, 60.0f, &started);
    if (started) {
        satellite.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;   /* which way */
        satellite.b = cy + random_range(-1.75f, -1.35f) * r;
        satellite.c = random_range(-0.25f, 0.25f) * r;                /* drift up or down */
    }
    if (p < 0.0f)
        return;
    const float x = cx + satellite.a * (p * 2.0f - 1.0f) * 2.3f * r, y = satellite.b + satellite.c * p;
    const float seen = fade(p, 0.12f);
    gfx_circle(x, y, 1.5f, with_alpha(SATELLITE_RGB, 0.85f * seen));
    if (fmodf(t, 1.3f) < 0.12f)
        gfx_circle(x, y, 2.5f, with_alpha(THEME_ACCENT, seen));
}

/* A cloud drifting slowly across, in front of the moon, its top lit by it. Opaque puffs in colours faded up from the
 * background, so where they overlap doesn't show. */
static void draw_cloud(float cx, float cy, float r, float t)
{
    bool started;
    const float p = progress(&cloud, t, 26.0f, 14.0f, 34.0f, &started);
    if (started) {
        cloud.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
        cloud.b = cy + random_range(-0.45f, 0.45f) * r;
        cloud.c = random_range(0.8f, 1.2f);   /* size */
    }
    if (p < 0.0f)
        return;
    const float s = r * cloud.c;
    const float x = cx + cloud.a * (p * 2.0f - 1.0f) * 2.3f * r, y = cloud.b + 3.0f * sinf(t * TAU / 7.0f);
    const float seen = fade(p, 0.2f);
    const uint32_t body = toward(CLOUD_RGB, seen), rim = toward(CLOUD_RIM_RGB, seen);
    /* Puffs: x, y, radius, in cloud sizes. The rim is each puff drawn a little higher, then covered by the body. */
    static const float puffs[][3] = { { -0.62f, 0.02f, 0.16f }, { -0.36f, -0.08f, 0.24f }, { 0.02f, -0.17f, 0.32f },
                                      { 0.38f, -0.05f, 0.22f }, { 0.64f, 0.04f, 0.14f } };
    const float lift = 0.035f * s;
    for (int i = 0; i < COUNT(puffs); i++)
        gfx_circle(x + puffs[i][0] * s, y + puffs[i][1] * s - lift, puffs[i][2] * s, rim);
    for (int i = 0; i < COUNT(puffs); i++)
        gfx_circle(x + puffs[i][0] * s, y + puffs[i][1] * s, puffs[i][2] * s, body);
    gfx_rect(x - 0.78f * s, y - 0.04f * s, 1.56f * s, 0.22f * s, 0.11f * s, body);   /* the flat bottom */
}

/* An aurora, rarely: two curtains of light rippling across the top of the sky, green and violet, bright at the bottom
 * and fading up. Columns side by side, each a vertical gradient, so nothing overlaps. */
static void draw_aurora(float cx, float cy, float r, float t)
{
    enum { COLUMNS = 60 };
    bool started;
    const float p = progress(&aurora, t, 20.0f, 360.0f, 600.0f, &started);
    if (started)
        aurora.a = random_range(0.0f, TAU);   /* its own ripple */
    if (p < 0.0f)
        return;
    const float seen = fade(p, 0.25f);
    const float left = cx - 2.4f * r, step = 4.8f * r / COLUMNS;
    for (int layer = 0; layer < 2; layer++) {
        const uint32_t rgb = layer ? AURORA_VIOLET : AURORA_GREEN;
        const float base = cy - (layer ? 1.55f : 1.25f) * r, strength = layer ? 0.26f : 0.38f;
        for (int i = 0; i < COLUMNS; i++) {
            const float u = (float)i / (COLUMNS - 1);
            /* The curtain's lower edge waves; its height and brightness ripple along it, both drifting with time. */
            const float wave = sinf(u * 5.0f + t * 0.7f + aurora.a + (float)layer) * 0.12f * r +
                               sinf(u * 11.0f - t * 1.1f) * 0.04f * r;
            const float ripple = 0.55f + 0.45f * sinf(u * 9.0f + t * 1.6f + aurora.a * 2.0f + (float)layer * 2.0f);
            const float edges = clamp01(u / 0.2f) * clamp01((1.0f - u) / 0.2f);   /* thinning out at both ends */
            const float h = (0.35f + 0.25f * ripple) * r;
            const float a = seen * strength * ripple * edges;
            const float x = left + step * (float)i, bottom = base + wave, soft = 0.12f * r;
            gfx_rect_v(x, bottom - h, step, h, 0.0f, with_alpha(rgb, 0.0f), with_alpha(rgb, a));
            gfx_rect_v(x, bottom, step, soft, 0.0f, with_alpha(rgb, a), with_alpha(rgb, 0.0f));   /* a soft lower edge */
        }
    }
}

/* A constellation: lines drawn from star to star, held a moment, faded. Dotted, as if traced. */
static void draw_constellation(float cx, float cy, float r, float t)
{
    /* Stars (by index) joined in order; -1 ends a figure. */
    static const int figures[][5] = { { 0, 5, 3, -1 }, { 4, 1, 6, 2, -1 }, { 0, 4, 1, -1 }, { 6, 2, 7, -1 } };
    bool started;
    const float p = progress(&constellation, t, 6.0f, 120.0f, 240.0f, &started);
    if (started)
        constellation.a = (float)(int)random_range(0.0f, (float)COUNT(figures) - 0.01f);
    if (p < 0.0f)
        return;
    const int *figure = figures[(int)constellation.a];
    int segments = 0;
    while (segments < 4 && figure[segments + 1] >= 0)
        segments++;
    const float drawn = clamp01(p / 0.4f) * (float)segments;   /* traced over the first 40% */
    const float alpha = 0.7f * clamp01((1.0f - p) / 0.3f);
    for (int s = 0; s < segments && (float)s < drawn; s++) {
        const float *a = stars[figure[s]], *b = stars[figure[s + 1]];
        const float x0 = cx + a[0] * r, y0 = cy + a[1] * r, x1 = cx + b[0] * r, y1 = cy + b[1] * r;
        const float length = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
        int dots = (int)(length / 9.0f);
        if (dots > 28)
            dots = 28;
        const float done = clamp01(drawn - (float)s);
        for (int k = 1; k < dots; k++) {   /* not on the stars themselves */
            const float u = (float)k / (float)dots;
            if (u > done)
                break;
            gfx_circle(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u, 1.4f, with_alpha(STAR_RGB, alpha));
        }
    }
    for (int s = 0; s <= segments && (float)s <= drawn + 0.001f; s++) {   /* the stars it reached, glowing */
        const float *a = stars[figure[s]];
        gfx_circle(cx + a[0] * r, cy + a[1] * r, 5.0f, with_alpha(STAR_RGB, 0.25f * alpha));
    }
}

/* A plane, high up: slow, a red and a green light at its wingtips and a white strobe flashing twice. */
static void draw_plane(float cx, float cy, float r, float t)
{
    bool started;
    const float p = progress(&plane, t, 22.0f, 150.0f, 300.0f, &started);
    if (started) {
        plane.a = random_range(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;   /* which way */
        plane.b = cy + random_range(-1.85f, -1.3f) * r;
        plane.c = random_range(-0.3f, 0.3f) * r;
    }
    if (p < 0.0f)
        return;
    const float x = cx + plane.a * (p * 2.0f - 1.0f) * 2.4f * r, y = plane.b + plane.c * p;
    const float seen = fade(p, 0.08f);
    /* Seen from below, flying right its left wing (red) is the far one, up; flying left, the near one. */
    gfx_circle(x - 5.0f, y - plane.a * 1.0f, 2.3f, with_alpha(PLANE_RED, 0.9f * seen));
    gfx_circle(x + 5.0f, y + plane.a * 1.0f, 2.3f, with_alpha(PLANE_GREEN, 0.9f * seen));
    const float beat = fmodf(t - plane.start, 1.4f);
    if (beat < 0.06f || (beat > 0.2f && beat < 0.26f)) {
        gfx_circle(x, y, 9.0f, with_alpha(0x00FFFFFF, 0.22f * seen));
        gfx_circle(x, y, 3.0f, with_alpha(0x00FFFFFF, seen));
    }
}

/* Fireflies: a few soft yellow-green lights wandering low beside the moon, each glowing and dimming on its own. */
static void draw_fireflies(float cx, float cy, float r, float t)
{
    enum { FLIES = 8 };
    bool started;
    const float p = progress(&fireflies, t, 14.0f, 150.0f, 270.0f, &started);
    if (started)
        fireflies.a = random_range(0.0f, TAU);
    if (p < 0.0f)
        return;
    const float seen = fade(p, 0.2f);
    for (int i = 0; i < FLIES; i++) {
        const float fi = (float)i, phase = fireflies.a + fi * 2.39f;
        /* Home spots either side of the moon, low; each loops lazily around its own. */
        const float home_x = (i % 2 ? 1.0f : -1.0f) * (1.35f + 0.22f * (float)(i / 2)) * r;
        const float home_y = (0.55f + 0.18f * sinf(phase * 3.0f)) * r;
        const float x = cx + home_x + 0.22f * r * sinf(t * (0.35f + 0.05f * fi) + phase);
        const float y = cy + home_y + 0.12f * r * sinf(t * (0.5f + 0.04f * fi) + phase * 1.7f);
        const float pulse = sinf(t * (1.1f + 0.13f * fi) + phase * 3.1f);
        const float glow = seen * clamp01(pulse * 1.4f);   /* dark half the time */
        if (glow <= 0.0f)
            continue;
        gfx_circle(x, y, 12.0f, with_alpha(FIREFLY_RGB, 0.07f * glow));   /* a soft glow: rings, fainter outward */
        gfx_circle(x, y, 8.0f, with_alpha(FIREFLY_RGB, 0.09f * glow));
        gfx_circle(x, y, 4.5f, with_alpha(FIREFLY_RGB, 0.16f * glow));
        gfx_circle(x, y, 2.4f, with_alpha(FIREFLY_RGB, glow));
    }
}

void moon_draw(float cx, float cy, float r)
{
    /* Frames since the last draw; after a gap of more than half a second (the sky wasn't shown), just one. */
    const uint32_t frame = gfx_frame(), step = frame - last_frame;
    sky_seconds += (float)(drawn && step <= 30 ? step : 1u) / FRAMES_PER_SECOND;
    last_frame = frame;
    drawn = true;
    const float t = sky_seconds;
    const float breath = 0.5f + 0.5f * sinf(t * TAU / 6.0f);   /* 0 to 1 over 6 s */
    const float my = cy + 5.0f * sinf(t * TAU / 9.0f);         /* the moon floats up and down over 9 s */
    /* Craters drift a little, as if the moon turned slowly. */
    const float turn = 0.04f * r * sinf(t * TAU / 23.0f);

    /* A blood moon, rarely: the moon warms to red and back over a few seconds. */
    bool started;
    const float bp = progress(&blood, t, 14.0f, 180.0f, 300.0f, &started);
    const float red = bp < 0.0f ? 0.0f : fade(bp, 0.35f);
    const uint32_t moon = mix(MOON, BLOOD_MOON, red), crater = mix(CRATER, BLOOD_CRATER, red);

    /* Back to front: sky, moon, what flies in front of it, cloud. */
    draw_aurora(cx, cy, r, t);
    draw_stars(cx, cy, r, t);
    draw_constellation(cx, cy, r, t);
    draw_comet(cx, cy, r, t);
    draw_satellite(cx, cy, r, t);
    draw_plane(cx, cy, r, t);
    draw_ufo(cx, cy, r, t);
    draw_shooting_star(cx, cy, r, t);
    draw_shower(cx, cy, r, t);

    gfx_circle(cx, my, r * (1.30f + 0.05f * breath), with_alpha(moon, (8.0f + 10.0f * breath) / 255.0f));
    gfx_circle(cx, my, r * 1.13f, with_alpha(moon, (14.0f + 10.0f * breath) / 255.0f));
    gfx_circle(cx, my, r, moon);
    gfx_circle(cx - r * 0.35f + turn, my - r * 0.25f, r * 0.20f, crater);
    gfx_circle(cx + r * 0.30f + turn, my + r * 0.10f, r * 0.13f, crater);
    gfx_circle(cx - r * 0.05f + turn, my + r * 0.45f, r * 0.10f, crater);

    draw_birds(cx, cy, r, t);
    draw_fireflies(cx, cy, r, t);
    draw_cloud(cx, cy, r, t);
}
