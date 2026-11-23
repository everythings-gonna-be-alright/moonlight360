/*
 * Checks the hybrid decoder's CPU half on the Mac: decodes a clip normally, then again with h264_xenon.c's hook on
 * and mc_ref.c standing in for the GPU, and compares every frame. Deblocking is off in both (the hybrid needs it off).
 *
 *     tools/hybrid_test/build.sh bench/data/clip1080_cavlc_s6_12500.h264
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/adler32.h>

#include "libavcodec/h264_xenon.h"
#include "mc_ref.h"

enum { MAX_FRAMES = 1024 };

/* The plain decode's frames, kept to find where a hybrid frame differs. */
static AVFrame *kept[MAX_FRAMES];
static uint32_t *debug_grid;
static int debug_pitch;

static void report_difference(int index, const AVFrame *f)
{
    const AVFrame *g = kept[index];
    if (!g)
        return;
    int shown = 0, count[3] = { 0, 0, 0 };
    for (int p = 0; p < 3; p++) {
        const int mbw = 16 >> !!p, w = p ? f->width / 2 : f->width, h = p ? f->height / 2 : f->height;
        for (int my = 0; my * mbw < h; my++)
            for (int mx = 0; mx * mbw < w; mx++) {
                int diff = 0, dx = 0, dy = 0;
                for (int y = my * mbw; y < (my + 1) * mbw && y < h && !diff; y++)
                    for (int x = mx * mbw; x < (mx + 1) * mbw && x < w; x++)
                        if (f->data[p][y * f->linesize[p] + x] != g->data[p][y * g->linesize[p] + x]) {
                            diff = f->data[p][y * f->linesize[p] + x] - g->data[p][y * g->linesize[p] + x];
                            dx = x - mx * mbw;
                            dy = y - my * mbw;
                            break;
                        }
                if (!diff)
                    continue;
                count[p]++;
                if (shown++ < 6) {
                    int gx, gy;
                    xenon_mv_decode(debug_grid[(my * 4) * debug_pitch + mx * 4], &gx, &gy);
                    uint32_t type;
                    const int mode = xenon_hybrid_debug(p ? mx : mx, p ? my : my, &type);
                    printf("  plane %d mb (%d,%d) first diff at (%d,%d) by %d; mode %d type 0x%x; grid mv %d,%d\n", p, mx,
                           my, dx, dy, diff, mode, type, gx, gy);
                }
            }
    }
    printf("  frame %d: macroblocks differing: luma %d, cb %d, cr %d\n", index, count[0], count[1], count[2]);
}

static uint8_t *data;
static size_t size;

/* The residual planes as gpumc.c lays them out for the GPU: 16-bit RGBA texels of four samples, tiled. */
uint32_t tiled_2d(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_texel_log2);
static uint8_t res_planes[3][480 * 1088 * 8];
static uint32_t res_offsets[3][32 * 480], res_texels[3], res_band[3];
static bool layout_tiled;

static void build_layout(void)
{
    for (int p = 0; p < 3; p++) {
        const uint32_t texels = p ? 240 : 480, pitch = (texels + 31) & ~31u;
        res_texels[p] = texels;
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t t = 0; t < texels; t++)
                res_offsets[p][y * texels + t] = tiled_2d(t, y, pitch, 3);
        res_band[p] = tiled_2d(0, 32, pitch, 3) - tiled_2d(0, 0, pitch, 3);
    }
}

static int residual_sample(int p, int x, int y, void *context)
{
    (void)context;
    if (!layout_tiled)
        return ((const int16_t *)res_planes[p])[y * (p ? 960 : 1920) + x];
    const uint8_t *texel = res_planes[p] + res_offsets[p][(y & 31) * res_texels[p] + (x >> 2)] + (uint32_t)(y >> 5) * res_band[p];
    return ((const int16_t *)texel)[x & 3];
}

/* Annex B access units, cut at each frame's first slice (first_mb_in_slice 0) or parameter set after a slice. */
static const uint8_t *next_au(const uint8_t *p, const uint8_t *end, const uint8_t **au_end)
{
    const uint8_t *start = p;
    int seen_slice = 0;
    while (p + 4 < end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            const int type = p[3] & 0x1F;
            const int slice = type == 1 || type == 5;
            const int first = slice && (p[4] & 0x80);
            const int param = type == 7 || type == 8 || type == 9;
            if ((first || param) && seen_slice) {
                *au_end = p > start && p[-1] == 0 ? p - 1 : p;
                return start;
            }
            if (slice)
                seen_slice = 1;
            p += 3;
        } else {
            p++;
        }
    }
    *au_end = end;
    return start;
}

static uint32_t checksum(const AVFrame *f)
{
    uint32_t sum = 1;
    for (int p = 0; p < 3; p++) {
        const int w = p ? (f->width + 1) / 2 : f->width, h = p ? (f->height + 1) / 2 : f->height;
        for (int y = 0; y < h; y++)
            sum = av_adler32_update(sum, f->data[p] + y * f->linesize[p], w);
    }
    return sum;
}

/* Decodes the clip; hybrid on: the hook, then mc_ref_predict and the replay after each packet. Fills sums. */
static int decode(int hybrid, uint32_t *sums, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = threads;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(ctx, codec, NULL) < 0)
        return -1;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    static uint32_t grid[(1920 / 4 + 8) * (1088 / 4 + 8)];
    const int grid_pitch = 1920 / 4 + 8;
    xenon_hybrid_set_grid(grid, grid_pitch, 1088 / 4 + 8);
    debug_grid = grid;
    debug_pitch = grid_pitch;
    /* Residuals to the stand-in GPU too (RESIDUAL=0 in the environment keeps the old replay of them), in the GPU's
     * tiled layout as gpumc.c sets it up (TILED=0: rows). */
    static uint8_t flags[120 * 68];
    const bool gpu_residual = hybrid && !(getenv("RESIDUAL") && !strcmp(getenv("RESIDUAL"), "0"));
    const bool tiled = !(getenv("TILED") && !strcmp(getenv("TILED"), "0"));
    build_layout();
    xenon_hybrid_set_residual((int16_t *)res_planes[0], 1920, (int16_t *)res_planes[1], (int16_t *)res_planes[2], 960,
                              gpu_residual ? flags : NULL, 120);
    if (tiled) {
        const uint32_t *const offsets[3] = { res_offsets[0], res_offsets[1], res_offsets[2] };
        xenon_hybrid_set_residual_layout(offsets, res_texels, res_band);
    } else {
        xenon_hybrid_set_residual_layout(NULL, NULL, NULL);
    }
    layout_tiled = tiled;
    xenon_hybrid_enable(hybrid);
    int frames = 0, gpu_frames = 0, replayed = 0;
    const uint8_t *p = data, *end = data + size;
    while (p < end) {
        const uint8_t *au_end;
        const uint8_t *au = next_au(p, end, &au_end);
        p = au_end;
        pkt->data = (uint8_t *)au;
        pkt->size = (int)(au_end - au);
        if (hybrid)
            xenon_hybrid_begin_frame();
        if (avcodec_send_packet(ctx, pkt) < 0) {
            printf("send_packet failed at frame %d\n", frames);
            return -1;
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (hybrid) {
                if (xenon_hybrid_failed())
                    printf("frame %d: the hybrid hook reported failure %d\n", frames, xenon_hybrid_failed());
                uint8_t *ref[3];
                if (xenon_hybrid_gpu_needed() && xenon_hybrid_reference(ref)) {
                    const int stride[3] = { frame->linesize[0], frame->linesize[1], frame->linesize[2] };
                    uint8_t *const dst[3] = { frame->data[0], frame->data[1], frame->data[2] };
                    mc_ref_predict((const uint8_t *const *)ref, stride, dst, stride, ctx->coded_width,
                                   ctx->coded_height, grid, grid_pitch);
                    if (gpu_residual)
                        mc_ref_add_residual(dst, stride, residual_sample, NULL, flags, 120, ctx->coded_width,
                                            ctx->coded_height);
                    gpu_frames++;
                }
                for (int i = 0; i < xenon_hybrid_contexts(); i++) {
                    const int n = xenon_hybrid_replay(i);
                    if (n < 0)
                        printf("frame %d: replay found the decoder on another picture\n", frames);
                    else
                        replayed += n;
                }
            }
            if (frames < MAX_FRAMES)
                sums[frames] = checksum(frame);
            if (!hybrid && frames < MAX_FRAMES)
                kept[frames] = av_frame_clone(frame);
            if (hybrid && frames == 1 && getenv("PROBE_MB")) {
                int mx, my;
                sscanf(getenv("PROBE_MB"), "%d,%d", &mx, &my);
                const AVFrame *r = kept[0], *g = kept[1];
                for (int b = 0; b < 16; b++) {
                    const int bx = mx * 4 + (b & 3), by = my * 4 + (b >> 2);
                    int mvx, mvy;
                    xenon_mv_decode(grid[by * grid_pitch + bx], &mvx, &mvy);
                    int bad = 0;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            const int px = bx * 4 + x, py = by * 4 + y;
                            const int pred = mc_ref_luma(r->data[0], r->linesize[0], ctx->coded_width, ctx->coded_height,
                                                         px + (mvx >> 2), py + (mvy >> 2), mvx & 3, mvy & 3);
                            bad += pred != g->data[0][py * g->linesize[0] + px];
                        }
                    printf("  block (%d,%d) mv %d,%d: %d of 16 luma samples differ from the plain decode", b & 3,
                           b >> 2, mvx, mvy, bad);
                    if (bad && b == 8) {
                        for (int sy = -80; sy <= 80; sy++)
                            for (int sx = -80; sx <= 80; sx++) {
                                int miss = 0;
                                for (int y = 0; y < 4 && !miss; y++)
                                    for (int x = 0; x < 4; x++) {
                                        const int px = bx * 4 + x, py = by * 4 + y;
                                        if (mc_ref_luma(r->data[0], r->linesize[0], ctx->coded_width, ctx->coded_height,
                                                        px + (sx >> 2), py + (sy >> 2), sx & 3, sy & 3) !=
                                            g->data[0][py * g->linesize[0] + px])
                                            miss = 1;
                                    }
                                if (!miss)
                                    printf(" [matches mv %d,%d]", sx, sy);
                            }
                    }
                    printf("\n");
                }
            }
            if (hybrid && frames < 3 && kept[frames] && sums[frames] != checksum(kept[frames]))
                report_difference(frames, frame);
            frames++;
            av_frame_unref(frame);
        }
    }
    if (hybrid)
        printf("hybrid: %d frames, %d predicted by the stand-in GPU, %d macroblocks replayed (%.0f a frame)\n", frames,
               gpu_frames, replayed, frames ? (double)replayed / frames : 0);
    xenon_hybrid_enable(0);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return frames;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s clip.h264 [threads]\n", argv[0]);
        return 2;
    }
    const int threads = argc > 2 ? atoi(argv[2]) : 6;
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = (size_t)ftell(f);
    rewind(f);
    data = malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    fread(data, 1, size, f);
    memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    fclose(f);

    static uint32_t plain[MAX_FRAMES], hybrid[MAX_FRAMES];
    const int n = decode(0, plain, threads);
    const int m = decode(1, hybrid, threads);
    if (n <= 0 || n != m) {
        printf("FAIL: %d frames plain, %d hybrid\n", n, m);
        return 1;
    }
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (plain[i] != hybrid[i]) {
            if (bad < 5)
                printf("frame %d differs: %08x plain, %08x hybrid\n", i, plain[i], hybrid[i]);
            bad++;
        }
    printf("%s: %d of %d frames identical (%d threads)\n", bad ? "FAIL" : "PASS", n - bad, n, threads);
    return bad != 0;
}
