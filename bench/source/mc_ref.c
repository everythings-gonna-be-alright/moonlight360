/* H.264 inter prediction for the hybrid decoder (P, one reference, no weights) from h264_xenon.c's motion vector grid:
 * the C model app/shaders/mc_*.frag follow line for line, checked against FFmpeg by tools/hybrid_test. */
#include "mc_ref.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int clip255(int v)
{
    return clampi(v, 0, 255);
}

typedef struct {
    const uint8_t *p;
    int stride, w, h;
} Plane;

/* A sample, its coordinates clamped to the picture: H.264's edge extension. */
static int at(const Plane *pl, int x, int y)
{
    return pl->p[clampi(y, 0, pl->h - 1) * pl->stride + clampi(x, 0, pl->w - 1)];
}

/* The 6-tap sums, unrounded: between (x, y) and (x + 1, y), and between (x, y) and (x, y + 1). */
static int tap_h(const Plane *pl, int x, int y)
{
    return at(pl, x - 2, y) - 5 * at(pl, x - 1, y) + 20 * at(pl, x, y) + 20 * at(pl, x + 1, y) - 5 * at(pl, x + 2, y) +
           at(pl, x + 3, y);
}

static int tap_v(const Plane *pl, int x, int y)
{
    return at(pl, x, y - 2) - 5 * at(pl, x, y - 1) + 20 * at(pl, x, y) + 20 * at(pl, x, y + 1) - 5 * at(pl, x, y + 2) +
           at(pl, x, y + 3);
}

static int avg(int a, int b)
{
    return (a + b + 1) >> 1;
}

/* The luma sample at integer (x, y) plus (fx, fy) quarters: H.264 8.4.2.2.1. */
int mc_ref_luma(const uint8_t *ref, int stride, int w, int h, int x, int y, int fx, int fy)
{
    const Plane pl = { ref, stride, w, h };
    const int G = at(&pl, x, y);
    if (!fx && !fy)
        return G;
    const int b = clip255((tap_h(&pl, x, y) + 16) >> 5);       /* half, right of G */
    const int hh = clip255((tap_v(&pl, x, y) + 16) >> 5);      /* half, below G */
    if (fy == 0)
        return fx == 1 ? avg(G, b) : fx == 2 ? b : avg(b, at(&pl, x + 1, y));
    if (fx == 0)
        return fy == 1 ? avg(G, hh) : fy == 2 ? hh : avg(hh, at(&pl, x, y + 1));
    const int m = clip255((tap_v(&pl, x + 1, y) + 16) >> 5);   /* half, below H */
    const int s = clip255((tap_h(&pl, x, y + 1) + 16) >> 5);   /* half, right of M */
    if (fx != 2 && fy != 2)
        return fx == 1 ? (fy == 1 ? avg(b, hh) : avg(hh, s)) : (fy == 1 ? avg(b, m) : avg(m, s));
    int j1 = 0;
    static const int taps[6] = { 1, -5, 20, 20, -5, 1 };
    for (int k = 0; k < 6; k++)
        j1 += taps[k] * tap_h(&pl, x, y - 2 + k);
    const int j = clip255((j1 + 512) >> 10);                   /* the centre half */
    if (fx == 2 && fy == 2)
        return j;
    if (fx == 2)
        return fy == 1 ? avg(b, j) : avg(j, s);
    return fx == 1 ? avg(hh, j) : avg(j, m);
}

/* The chroma sample at integer (x, y) plus (dx, dy) eighths: H.264 8.4.2.2.2. */
int mc_ref_chroma(const uint8_t *ref, int stride, int w, int h, int x, int y, int dx, int dy)
{
    const Plane pl = { ref, stride, w, h };
    return ((8 - dx) * (8 - dy) * at(&pl, x, y) + dx * (8 - dy) * at(&pl, x + 1, y) + (8 - dx) * dy * at(&pl, x, y + 1) +
            dx * dy * at(&pl, x + 1, y + 1) + 32) >> 6;
}

/* The grid's float (h264_xenon.h): (mvx + 2048) * 4096 + (mvy + 2048). */
static void grid_mv(const uint32_t *grid, int pitch, int bx, int by, int *mvx, int *mvy)
{
    union { uint32_t u; float f; } v = { .u = grid[by * pitch + bx] };
    const int packed = (int)v.f;
    *mvx = packed / 4096 - 2048;
    *mvy = packed % 4096 - 2048;
}

void mc_ref_predict(const uint8_t *const ref[3], const int ref_stride[3], uint8_t *const dst[3], const int dst_stride[3],
                    int width, int height, const uint32_t *grid, int grid_pitch)
{
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            int mvx, mvy;
            grid_mv(grid, grid_pitch, x >> 2, y >> 2, &mvx, &mvy);
            dst[0][y * dst_stride[0] + x] =
                (uint8_t)mc_ref_luma(ref[0], ref_stride[0], width, height, x + (mvx >> 2), y + (mvy >> 2), mvx & 3, mvy & 3);
        }
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < height / 2; y++)
            for (int x = 0; x < width / 2; x++) {
                int mvx, mvy;
                grid_mv(grid, grid_pitch, x >> 1, y >> 1, &mvx, &mvy);
                dst[p][y * dst_stride[p] + x] = (uint8_t)mc_ref_chroma(ref[p], ref_stride[p], width / 2, height / 2,
                                                                       x + (mvx >> 3), y + (mvy >> 3), mvx & 7, mvy & 7);
            }
}

void mc_ref_add_residual(uint8_t *const dst[3], const int dst_stride[3], McResidualSample sample, void *context,
                         const uint8_t *flags, int flags_pitch, int width, int height)
{
    for (int p = 0; p < 3; p++) {
        const int w = p ? width / 2 : width, h = p ? height / 2 : height, mb = p ? 8 : 16;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (flags[(y / mb) * flags_pitch + x / mb]) {
                    uint8_t *d = &dst[p][y * dst_stride[p] + x];
                    *d = (uint8_t)clip255(*d + sample(p, x, y, context));
                }
    }
}
