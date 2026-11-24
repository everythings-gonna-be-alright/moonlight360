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
                      CLOUD_RGB = 0x0026262B, CLOUD_RIM_RGB = 0x003E3D40;
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

static uint32_t seed = 0x9E3779B9u;

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
    if (p < 0.0f)
        return;
    const float eased = 1.0f - (1.0f - p) * (1.0f - p);   /* fast at first, slowing as it burns out */
    const float hx = shooting.a + shooting.c * eased, hy = shooting.b + shooting.d * eased;
    const float bright = fade(p, 0.25f);
    enum { TAIL = 36 };
    for (int k = TAIL - 1; k >= 0; k--) {   /* back to front, the head last */
        const float back = (float)k / TAIL;
        const float x = hx - shooting.c * 0.30f * back, y = hy - shooting.d * 0.30f * back;
        const float left = 1.0f - back;
        gfx_circle(x, y, 0.7f + 1.8f * left, with_alpha(STAR_RGB, bright * left * left));
    }
    gfx_circle(hx, hy, 5.0f, with_alpha(STAR_RGB, 0.22f * bright));   /* the head's glow */
    gfx_circle(hx, hy, 2.6f, with_alpha(0x00FFFFFF, bright));
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

void moon_draw(float cx, float cy, float r)
{
    const float t = (float)gfx_frame() / FRAMES_PER_SECOND;
    const float breath = 0.5f + 0.5f * sinf(t * TAU / 6.0f);   /* 0 to 1 over 6 s */
    const float my = cy + 5.0f * sinf(t * TAU / 9.0f);         /* the moon floats up and down over 9 s */
    /* Craters drift a little, as if the moon turned slowly. */
    const float turn = 0.04f * r * sinf(t * TAU / 23.0f);

    /* Back to front: sky, moon, cloud. */
    draw_stars(cx, cy, r, t);
    draw_satellite(cx, cy, r, t);
    draw_shooting_star(cx, cy, r, t);

    gfx_circle(cx, my, r * (1.30f + 0.05f * breath), with_alpha(MOON, (8.0f + 10.0f * breath) / 255.0f));
    gfx_circle(cx, my, r * 1.13f, with_alpha(MOON, (14.0f + 10.0f * breath) / 255.0f));
    gfx_circle(cx, my, r, MOON);
    gfx_circle(cx - r * 0.35f + turn, my - r * 0.25f, r * 0.20f, CRATER);
    gfx_circle(cx + r * 0.30f + turn, my + r * 0.10f, r * 0.13f, CRATER);
    gfx_circle(cx - r * 0.05f + turn, my + r * 0.45f, r * 0.10f, CRATER);

    draw_cloud(cx, cy, r, t);
}
