/* Checks the Xenon VMX functions (third_party/ffmpeg/libavcodec/ppc/h264_xenon_vmx.h) against FFmpeg's C code, which
 * is compiled in from FFmpeg's own templates. Built for big-endian PowerPC and run under QEMU: dev/vmx_test/run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIT_DEPTH 8
#include "libavcodec/h264chroma_template.c"
#include "libavcodec/h264dsp_template.c"

#include "libavcodec/ppc/h264_xenon_vmx.h"

enum { STRIDE = 64, ROWS = 40, SIZE = STRIDE * ROWS, GUARD = 64 };

static uint32_t seed = 360;
static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* Two copies of a picture between guard bands: the C code works on one, the vector code on the other. */
static uint8_t buf_c[GUARD + SIZE + GUARD] __attribute__((aligned(16)));
static uint8_t buf_v[GUARD + SIZE + GUARD] __attribute__((aligned(16)));
static uint8_t source[GUARD + SIZE + GUARD] __attribute__((aligned(16)));

/* Flat areas, gradients and noise: deblocking only acts on small steps, so pure noise would test little. */
static void fill(uint8_t *p, size_t n)
{
    const int kind = (int)(rnd() % 3), base = (int)(rnd() % 256), spread = 1 + (int)(rnd() % 12);
    for (size_t i = 0; i < n; i++) {
        int v = kind == 0 ? (int)(rnd() % 256) : base + (int)(rnd() % (unsigned)spread) - spread / 2;
        if (kind == 2)
            v += (int)(i % STRIDE) / 4;
        p[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static int failures;

static void compare(const char *what, int a, int b, int c, int d)
{
    if (memcmp(buf_c, buf_v, sizeof buf_c) == 0)
        return;
    if (failures++ < 10) {
        size_t i = 0;
        while (buf_c[i] == buf_v[i])
            i++;
        printf("MISMATCH %s (%d, %d, %d, %d): byte %zu (row %zu, column %zu): C %d, VMX %d\n", what, a, b, c, d,
               i, (i - GUARD) / STRIDE, (i - GUARD) % STRIDE, buf_c[i], buf_v[i]);
    }
}

static void test_mc8(int avg, int runs)
{
    static const int heights[] = { 2, 4, 8, 16 };
    for (int run = 0; run < runs; run++) {
        fill(source, sizeof source);
        fill(buf_c, sizeof buf_c);
        memcpy(buf_v, buf_c, sizeof buf_c);
        const int x = (int)(rnd() % 8), y = (int)(rnd() % 8), h = heights[rnd() % 4];
        const int dst_at = GUARD + STRIDE * (int)(2 + rnd() % 8) + 8 * (int)(rnd() % 6);   /* 8-aligned */
        const int src_at = GUARD + STRIDE * (int)(2 + rnd() % 8) + (int)(rnd() % 40);      /* any alignment */
        if (avg) {
            avg_h264_chroma_mc8_8_c(buf_c + dst_at, source + src_at, STRIDE, h, x, y);
            xv_h264_chroma_mc8(buf_v + dst_at, source + src_at, STRIDE, h, x, y, 1);
        } else {
            put_h264_chroma_mc8_8_c(buf_c + dst_at, source + src_at, STRIDE, h, x, y);
            xv_h264_chroma_mc8(buf_v + dst_at, source + src_at, STRIDE, h, x, y, 0);
        }
        compare(avg ? "avg_h264_chroma_mc8" : "put_h264_chroma_mc8", x, y, h, src_at % 16);
    }
}

/* Every weight against every pair of pixel steps, exactly: the arithmetic's full range. */
static void test_mc8_extremes(void)
{
    static const uint8_t levels[] = { 0, 1, 127, 128, 254, 255 };
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            for (unsigned combo = 0; combo < 6 * 6 * 6 * 6; combo++) {
                const uint8_t a = levels[combo % 6], b = levels[combo / 6 % 6], c = levels[combo / 36 % 6],
                              d = levels[combo / 216 % 6];
                for (int row = 0; row < ROWS; row++)
                    for (int col = 0; col < STRIDE; col++)
                        source[GUARD + row * STRIDE + col] = row & 1 ? (col & 1 ? d : c) : (col & 1 ? b : a);
                memset(buf_c, 9, sizeof buf_c);
                memset(buf_v, 9, sizeof buf_v);
                put_h264_chroma_mc8_8_c(buf_c + GUARD + STRIDE * 4, source + GUARD + STRIDE * 4 + 3, STRIDE, 8, x, y);
                xv_h264_chroma_mc8(buf_v + GUARD + STRIDE * 4, source + GUARD + STRIDE * 4 + 3, STRIDE, 8, x, y, 0);
                compare("put_h264_chroma_mc8 extremes", x, y, (int)combo, 0);
            }
}

static void test_deblock(int vertical_edge, int intra, int runs)
{
    for (int run = 0; run < runs; run++) {
        fill(buf_c, sizeof buf_c);
        if (rnd() % 4 == 0)   /* a clean step across the picture: an edge that really gets filtered */
            for (int i = GUARD; i < GUARD + SIZE; i++)
                buf_c[i] = (uint8_t)(((i - GUARD) % STRIDE < 28 || (i - GUARD) / STRIDE < 12 ? 100 : 100 + rnd() % 9));
        memcpy(buf_v, buf_c, sizeof buf_c);
        /* alpha to 255, beta to 18, tc 0 (off) to 26, as H.264's tables give them; sometimes out of range. */
        const int alpha = rnd() % 8 == 0 ? (int)(rnd() % 256) : 1 + (int)(rnd() % 40);
        const int beta = 1 + (int)(rnd() % 18);
        int8_t tc[4];
        for (int i = 0; i < 4; i++)
            tc[i] = (int8_t)(rnd() % 3 == 0 ? 0 : rnd() % 27);
        if (rnd() % 16 == 0)
            tc[rnd() % 4] = -1;
        /* A horizontal edge sits on an 8-aligned pixel; a vertical one on a 4-aligned column. */
        const int at = GUARD + STRIDE * (int)(8 + rnd() % 8) + (vertical_edge ? 4 : 8) * (int)(1 + rnd() % 5);
        if (vertical_edge && intra) {
            h264_h_loop_filter_chroma_intra_8_c(buf_c + at, STRIDE, alpha, beta);
            xv_h264_h_loop_filter_chroma(buf_v + at, STRIDE, alpha, beta, NULL);
        } else if (vertical_edge) {
            h264_h_loop_filter_chroma_8_c(buf_c + at, STRIDE, alpha, beta, tc);
            xv_h264_h_loop_filter_chroma(buf_v + at, STRIDE, alpha, beta, tc);
        } else if (intra) {
            h264_v_loop_filter_chroma_intra_8_c(buf_c + at, STRIDE, alpha, beta);
            xv_h264_v_loop_filter_chroma(buf_v + at, STRIDE, alpha, beta, NULL);
        } else {
            h264_v_loop_filter_chroma_8_c(buf_c + at, STRIDE, alpha, beta, tc);
            xv_h264_v_loop_filter_chroma(buf_v + at, STRIDE, alpha, beta, tc);
        }
        compare(vertical_edge ? (intra ? "h_loop_filter_chroma_intra" : "h_loop_filter_chroma")
                              : (intra ? "v_loop_filter_chroma_intra" : "v_loop_filter_chroma"),
                alpha, beta, tc[0], at % 16);
    }
}

int main(void)
{
    const int runs = 200000;
    test_mc8(0, runs);
    test_mc8(1, runs);
    test_mc8_extremes();
    for (int vertical_edge = 0; vertical_edge < 2; vertical_edge++)
        for (int intra = 0; intra < 2; intra++)
            test_deblock(vertical_edge, intra, runs);
    if (failures) {
        printf("FAIL: %d mismatches\n", failures);
        return 1;
    }
    printf("PASS: chroma mc8 (put, avg, every weight at the extremes) and chroma deblocking (4 filters), "
           "%d random cases each, identical to FFmpeg's C code\n", runs);
    return 0;
}
