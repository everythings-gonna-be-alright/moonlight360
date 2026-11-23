/*
 * hybench: the hybrid H.264 decoder on the console. Each clip is decoded twice, plainly (FFmpeg, 6 slice threads, no
 * deblocking) and hybrid (FFmpeg parses, the GPU predicts, FFmpeg replays the rest on 6 threads); every hybrid frame
 * must match the plain one, the first few predictions are checked pixel by pixel against bench/source/mc_ref.c, and
 * each stage is timed. Logs to hybench.log and returns to devlaunch when started from it.
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <time.h>

#include <math.h>

#include <libavcodec/avcodec.h>
#include <libavutil/adler32.h>
#include <libavutil/buffer.h>

#include "devreturn.h"
#include "gfx.h"
#include "gpumc.h"
#include "mc_ref.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/present.h"
#include "xsdk/text.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

/* h264_xenon.h: FFmpeg's internal header, its prototypes. */
void xenon_hybrid_set_grid(uint32_t *mv_grid, int pitch_blocks, int rows_blocks);
void xenon_hybrid_set_residual_layout(const uint32_t *const offsets[3], const uint32_t texels[3],
                                      const uint32_t band[3]);
void xenon_hybrid_enable(int on);
void xenon_hybrid_set_residual(int16_t *luma, int luma_pitch, int16_t *cb, int16_t *cr, int chroma_pitch,
                               uint8_t *flags, int flags_pitch_bytes);
void xenon_hybrid_begin_frame(void);
int xenon_hybrid_gpu_needed(void);
int xenon_hybrid_failed(void);
int xenon_hybrid_reference(uint8_t *planes[3]);
int xenon_hybrid_contexts(void);
int xenon_hybrid_replay(int index);

extern const uint8_t cavlc12_start[], cavlc12_end[], cavlc22_start[], cavlc22_end[], cabac22_start[], cabac22_end[];

typedef struct {
    const char *name;
    const uint8_t *start, *end;
} Clip;
static const Clip clips[] = {
    { "CAVLC 11.5 Mbps (a 1080p60 25 Mbps frame)", cavlc12_start, cavlc12_end },
    { "CAVLC 22.6 Mbps", cavlc22_start, cavlc22_end },
    { "CABAC 22.5 Mbps", cabac22_start, cabac22_end },
};

enum { THREADS = 6, MAX_FRAMES = 128, CHECKED_FRAMES = 3 };

clock_t times(struct tms *buf)
{
    (void)buf;
    return (clock_t)-1;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* FFmpeg's slice workers on hw 3, 4, 5, 0, 1; the bench thread decodes on hw 2, as the app does. */
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static atomic_int next_worker;
static bool placing;

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    static const uint32_t hw[] = { 3, 4, 5, 0, 1 };
    int ret = __real_pthread_create(thread, attr, start, arg);
    if (ret == 0 && placing)
        xsdk_thread_place(xsdk_pthread_handle(*thread), hw[atomic_fetch_add(&next_worker, 1) % 5]);
    return ret;
}

/* ---- the watchdog: a hung GPU can block the bench for good, so it reboots the console and the log says where ----
 * The console then boots into devlaunch (DashLaunch's Default), which uploads hybench.log. */

enum { STALL_SECONDS = 30, RUN_SECONDS = 300 };
static const char *volatile stage = "starting";
static atomic_uint_fast64_t last_progress_us;

/* The steps so far, on the TV: a hang leaves the screen on the step that caused it. */
static bool screen_open;
static uint32_t screen_w, screen_h;
static char screen_lines[16][160];
static int screen_count;

static void show_screen(void)
{
    if (!screen_open)
        return;
    uint32_t address;
    if (!NT_SUCCESS(xsdk_present_begin(&address, 1000)))
        return;
    xsdk_gpu_set_target(screen_w, screen_h);
    xsdk_text_draw(64, 48, 2, 0xFFFFFFFF, "hybench: the GPU decoder test");
    for (int i = 0; i < screen_count; i++)
        xsdk_text_draw(64, 120 + i * 28, 1, i == screen_count - 1 ? 0xFF8EB0FF : 0xFFA7B3C5, screen_lines[i]);
    xsdk_text_draw(64, (int32_t)screen_h - 60, 1, 0xFF6B788C,
                   "If the GPU hangs it reboots the console by itself; devlaunch then sends the log to the Mac.");
    xsdk_gpu_resolve(address, screen_w, screen_h, 0xFF0C1016);
    xsdk_present_end();
}

static void progress(const char *what)
{
    static char kept[160];
    snprintf(kept, sizeof kept, "%s", what);
    stage = kept;
    atomic_store(&last_progress_us, now_us());
    if (screen_count && !strcmp(screen_lines[screen_count - 1], kept))
        return;
    if (screen_count == 16) {
        memmove(screen_lines[0], screen_lines[1], sizeof screen_lines[0] * 15);
        screen_count--;
    }
    snprintf(screen_lines[screen_count++], sizeof screen_lines[0], "%s", kept);
    show_screen();
}

static void reboot_console(void)
{
    HANDLE kernel = 0;
    void *address = NULL;
    const uint32_t found = XexGetModuleHandle("xboxkrnl.exe", &kernel);
    const NTSTATUS status = found == 0 ? XexGetProcedureAddress(kernel, 0x28, &address) : (NTSTATUS)found;
    if (found == 0 && NT_SUCCESS(status) && address)
        ((void (*)(uint32_t))address)(1);   /* HalReturnToFirmware(1): reboot, as devlaunch does */
}

static void *watchdog(void *arg)
{
    (void)arg;
    const uint64_t start = now_us();
    for (;;) {
        xsdk_thread_sleep(1000);
        const uint64_t now = now_us();
        const bool stalled = now - atomic_load(&last_progress_us) > STALL_SECONDS * 1000000ull;
        if (stalled || now - start > RUN_SECONDS * 1000000ull) {
            printf("\nWATCHDOG: %s at \"%s\"; rebooting the console (devlaunch then uploads this log)\n",
                   stalled ? "no progress for 30 s" : "the run passed 5 minutes", stage);
            fflush(stdout);
            xsdk_thread_sleep(500);
            reboot_console();
            printf("WATCHDOG: the reboot call failed\n");
            fflush(stdout);
            return NULL;
        }
    }
}

/* ---- frames in memory the GPU reads (as app/src/video.c) ---- */

static AVBufferPool *pools[3];
static int pool_pitch[3];

static void physical_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    MmFreePhysicalMemory(0, data);
}

static AVBufferRef *physical_alloc(void *opaque, size_t size)
{
    (void)opaque;
    const uint32_t page = 64 * 1024, rounded = ((uint32_t)size + page - 1) & ~(page - 1);
    uint8_t *memory = MmAllocatePhysicalMemoryEx(0, rounded, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 4096);
    if (!memory)
        return NULL;
    AVBufferRef *b = av_buffer_create(memory, size, physical_free, NULL, 0);
    if (!b)
        MmFreePhysicalMemory(0, memory);
    return b;
}

static int get_buffer(AVCodecContext *c, AVFrame *f, int flags)
{
    (void)flags;
    int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
    avcodec_align_dimensions2(c, &w, &h, align);
    for (int p = 0; p < 3; p++) {
        if (!pools[p]) {
            XsdkTexture t;
            xsdk_texture_init(&t, XSDK_FORMAT_R8, (uint32_t)(p ? w / 2 : w), (uint32_t)(p ? h / 2 : h), 1, false);
            pool_pitch[p] = (int)t.pitch;
            pools[p] = av_buffer_pool_init2((size_t)t.pitch * (size_t)(p ? h / 2 : h), NULL, physical_alloc, NULL);
        }
        f->buf[p] = av_buffer_pool_get(pools[p]);
        if (!f->buf[p])
            return AVERROR(ENOMEM);
        f->data[p] = f->buf[p]->data;
        f->linesize[p] = pool_pitch[p];
    }
    f->extended_data = f->data;
    return 0;
}

static void write_out_frame(const AVFrame *f, int coded_height)
{
    for (int p = 0; p < 3; p++) {
        const size_t bytes = (size_t)f->linesize[p] * (size_t)(p ? coded_height / 2 : coded_height);
        for (uintptr_t a = (uintptr_t)f->data[p] & ~(uintptr_t)127; a < (uintptr_t)f->data[p] + bytes; a += 128)
            __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

/* ---- replay threads, one per hardware thread ---- */

static sem_t replay_start[THREADS], replay_done;
static atomic_int replay_next, replay_count, replay_errors;

static void *replay_worker(void *arg)
{
    (void)arg;
    const int index = (int)(intptr_t)arg;
    for (;;) {
        sem_wait(&replay_start[index]);
        for (int i; (i = atomic_fetch_add(&replay_next, 1)) < atomic_load(&replay_count);)
            if (xenon_hybrid_replay(i) < 0)
                atomic_fetch_add(&replay_errors, 1);
        sem_post(&replay_done);
    }
    return NULL;
}

static void start_replay_threads(void)
{
    sem_init(&replay_done, 0, 0);
    for (int i = 0; i < THREADS; i++) {
        sem_init(&replay_start[i], 0, 0);
        pthread_t t;
        __real_pthread_create(&t, NULL, replay_worker, (void *)(intptr_t)i);
        xsdk_thread_place(xsdk_pthread_handle(t), (uint32_t)i);
    }
}

static void replay_all(void)
{
    atomic_store(&replay_next, 0);
    atomic_store(&replay_count, xenon_hybrid_contexts());
    for (int i = 0; i < THREADS; i++)
        sem_post(&replay_start[i]);
    for (int i = 0; i < THREADS; i++)
        sem_wait(&replay_done);
}

/* ---- decoding ---- */

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

typedef struct {
    uint64_t parse, flush, gpu, luma, detile, replay, writeout, total, pass[5];
    int frames, gpu_frames, mismatches;
} Totals;

static uint8_t *scratch[3];

/* mc_ref's prediction plus the residual FFmpeg wrote for the GPU: what the GPU's output must be. */
static int residual_sample(int plane, int x, int y, void *context)
{
    (void)context;
    return gpumc_residual_sample(plane, x, y);
}

static void add_residual(uint8_t *const dst[3], const int pitch[3], int w, int h)
{
    int16_t *res[3];
    int res_pitch[2], flags_pitch;
    uint8_t *flags;
    gpumc_residual(res, res_pitch, &flags, &flags_pitch);
    mc_ref_add_residual(dst, pitch, residual_sample, NULL, flags, flags_pitch, w, h);
}

static bool staged, gpu_usable = true, gpu_hung;

/* Compares a plane with what it should be: returns the differing pixels; prints the first. */
static int compare_plane(const char *what, const uint8_t *got, const uint8_t *want, int pitch, int w, int h)
{
    int bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (got[y * pitch + x] != want[y * pitch + x] && !bad++)
                printf("  %s: first difference at (%d,%d): %d, should be %d\n", what, x, y, got[y * pitch + x],
                       want[y * pitch + x]);
    printf("  %s: %s (%d of %d pixels differ)\n", what, bad ? "WRONG" : "exact", bad, w * h);
    fflush(stdout);
    return bad;
}

/* The GPU passes one at a time, simplest first, on the first predicted frame; stops at the first that hangs. */
static void run_stages(uint8_t *const ref[3], AVFrame *f, int w, int h, const uint32_t *grid, int grid_pitch)
{
    uint8_t *const dst[3] = { f->data[0], f->data[1], f->data[2] };
    const int pitch[3] = { f->linesize[0], f->linesize[1], f->linesize[2] };
    if (!scratch[0])
        for (int p = 0; p < 3; p++)
            scratch[p] = malloc((size_t)pitch[p] * (size_t)(p ? h / 2 : h));
    progress("C reference prediction for the GPU steps");
    mc_ref_predict((const uint8_t *const *)ref, pitch, scratch, pitch, w, h, grid, grid_pitch);
    add_residual(scratch, pitch, w, h);
    printf("GPU steps on the first predicted frame:\n");
    fflush(stdout);
    static const char *const tries[GPUMC_TRIES] = {
        "the grid texture (R32F), two textures bound", "a 64x64 R8 texture", "a 1920x1088 R8 texture, linear filter",
        "a 1920x1088 R8 texture, point sampling", "the frame where FFmpeg decoded it",
        "the frame, into the full 480x1088 target" };
    for (int t = 0; t < GPUMC_TRIES; t++) {
        char line[160];
        snprintf(line, sizeof line, "GPU try %d of %d: %s", t + 1, GPUMC_TRIES, tries[t]);
        progress(line);
        const int result = gpumc_try(t, ref, pitch);
        printf("  try %d, %s: %s\n", t + 1, tries[t],
               result > 0 ? "finished" : result == 0 ? "the GPU HUNG" : "couldn't be set up");
        fflush(stdout);
        if (result == 0) {
            gpu_usable = false;
            gpu_hung = true;
            return;
        }
        if (result < 0) {
            gpu_usable = false;
            return;
        }
    }
    progress("GPU step: copy pass");
    if (!gpumc_test(GPUMC_TEST_COPY, ref, dst, pitch)) {
        printf("  copy pass: the GPU HUNG\n");
        gpu_usable = false;
        gpu_hung = true;
        return;
    }
    compare_plane("copy pass (luma, no vectors)", dst[0], ref[0], pitch[0], w, h);
    progress("GPU step: chroma pass");
    if (!gpumc_test(GPUMC_TEST_CHROMA, ref, dst, pitch)) {
        printf("  chroma pass: the GPU HUNG\n");
        gpu_usable = false;
        gpu_hung = true;
        return;
    }
    compare_plane("chroma pass (Cb)", dst[1], scratch[1], pitch[1], w / 2, h / 2);
    progress("GPU step: luma pass");
    if (!gpumc_test(GPUMC_TEST_LUMA, ref, dst, pitch)) {
        printf("  luma pass: the GPU HUNG\n");
        gpu_usable = false;
        gpu_hung = true;
        return;
    }
    compare_plane("luma pass", dst[0], scratch[0], pitch[0], w, h);
}

/* The GPU's prediction against mc_ref's, plane by plane: differing pixels, and the first one. */
static void check_prediction(int frame_no, uint8_t *const ref[3], const AVFrame *f, int w, int h, const uint32_t *grid,
                             int grid_pitch)
{
    const int pitch[3] = { f->linesize[0], f->linesize[1], f->linesize[2] };
    if (!scratch[0])
        for (int p = 0; p < 3; p++)
            scratch[p] = malloc((size_t)pitch[p] * (size_t)(p ? h / 2 : h));
    mc_ref_predict((const uint8_t *const *)ref, pitch, scratch, pitch, w, h, grid, grid_pitch);
    add_residual(scratch, pitch, w, h);
    for (int p = 0; p < 3; p++) {
        const int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
        int bad = 0, fx = -1, fy = -1, got = 0, want = 0;
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                const int a = f->data[p][y * pitch[p] + x], b = scratch[p][y * pitch[p] + x];
                if (a != b && !bad++) {
                    fx = x;
                    fy = y;
                    got = a;
                    want = b;
                }
            }
        printf("  frame %d plane %d: GPU prediction %s", frame_no, p, bad ? "DIFFERS" : "matches mc_ref");
        if (bad) {
            const int bx = p ? fx / 2 : fx / 4, by = p ? fy / 2 : fy / 4;
            union { uint32_t u; float f; } v = { .u = grid[by * grid_pitch + bx] };
            const int packed = (int)v.f;
            printf(" at %d of %d pixels, first (%d,%d): %d, mc_ref %d; vector %d,%d", bad, pw * ph, fx, fy, got, want,
                   packed / 4096 - 2048, packed % 4096 - 2048);
        }
        printf("\n");
    }
}

static int decode(const Clip *clip, bool hybrid, uint32_t *sums, Totals *t)
{
    memset(t, 0, sizeof *t);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = THREADS;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->skip_loop_filter = AVDISCARD_ALL;
    ctx->get_buffer2 = get_buffer;
    atomic_store(&next_worker, 0);
    placing = true;
    if (avcodec_open2(ctx, codec, NULL) < 0)
        return -1;
    placing = false;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int grid_pitch = 0, grid_rows = 0;
    uint32_t *grid = gpumc_grid(&grid_pitch, &grid_rows);
    xenon_hybrid_set_grid(grid, grid_pitch, grid_rows);
    int16_t *res[3];
    int res_pitch[2], flags_pitch;
    uint8_t *flags;
    gpumc_residual(res, res_pitch, &flags, &flags_pitch);
    xenon_hybrid_set_residual(res[0], res_pitch[0], res[1], res[2], res_pitch[1], hybrid ? flags : NULL, flags_pitch);
    const uint32_t *layout_offsets[3];
    uint32_t layout_texels[3], layout_band[3];
    gpumc_residual_layout(layout_offsets, layout_texels, layout_band);
    xenon_hybrid_set_residual_layout(layout_offsets, layout_texels, layout_band);
    xenon_hybrid_enable(hybrid);
    int frames = 0;
    const uint8_t *p = clip->start;
    while (p < clip->end && gpu_usable) {
        const uint8_t *au_end;
        const uint8_t *au = next_au(p, clip->end, &au_end);
        p = au_end;
        pkt->data = (uint8_t *)au;
        pkt->size = (int)(au_end - au);
        progress(hybrid ? "hybrid decoding" : "plain decoding");   /* the same line each frame: shown once */
        const uint64_t t0 = now_us();
        if (hybrid)
            xenon_hybrid_begin_frame();
        if (avcodec_send_packet(ctx, pkt) < 0)
            printf("  send_packet failed at frame %d\n", frames);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            const uint64_t t1 = now_us();
            t->parse += t1 - t0;
            uint64_t t2 = t1, t3 = t1;
            if (hybrid) {
                uint8_t *ref[3];
                if (xenon_hybrid_failed())
                    printf("  frame %d: the hook reported a failure\n", frames);
                if (xenon_hybrid_gpu_needed() && xenon_hybrid_reference(ref) && !staged) {
                    staged = true;
                    run_stages(ref, frame, ctx->coded_width, ctx->coded_height, grid, grid_pitch);
                }
                if (!gpu_usable) {
                    printf("  the GPU hung: the rest is skipped\n");
                    av_frame_unref(frame);
                    break;
                }
                if (xenon_hybrid_gpu_needed() && xenon_hybrid_reference(ref)) {
                    /* Frame 1 un-tiles with the SDK's download too, as a check on the fast one: both must match mc_ref. */
                    GpuMcTimes g = { .split = true, .sdk_download = t->gpu_frames == 0 };
                    uint8_t *const dst[3] = { frame->data[0], frame->data[1], frame->data[2] };
                    progress("GPU prediction of a frame");
                    if (!gpumc_predict(ref, dst, frame->linesize, &g)) {
                        printf("  frame %d: gpumc_predict failed\n", frames);
                        gpu_usable = false;
                        gpu_hung = true;
                    }
                    t->flush += g.flush_us;
                    t->gpu += g.gpu_us;
                    t->luma += g.luma_us;
                    for (int i = 0; i < 5; i++)
                        t->pass[i] += g.pass_us[i];
                    t->detile += g.detile_us;
                    t->gpu_frames++;
                    if (t->gpu_frames <= CHECKED_FRAMES)
                        check_prediction(frames, ref, frame, ctx->coded_width, ctx->coded_height, grid, grid_pitch);
                }
                t2 = now_us();
                progress("replay");
                replay_all();
                t3 = now_us();
                t->replay += t3 - t2;
            }
            write_out_frame(frame, ctx->coded_height);   /* the next picture's reference, for the GPU */
            const uint64_t t4 = now_us();
            t->writeout += t4 - t3;
            t->total += t4 - t0;
            if (frames < MAX_FRAMES)
                sums[frames] = checksum(frame);
            frames++;
            av_frame_unref(frame);
        }
    }
    t->frames = frames;
    xenon_hybrid_enable(0);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    return frames;
}

/* ---- the menus' shape shader, checked on the console ---- */

/* What app/shaders/shape.frag should give at pixel (x, y) for a shape at (sx, sy, w, h, radius), as coverage 0-1. */
static float expected_coverage(int x, int y, float sx, float sy, float w, float h, float radius)
{
    const float px = (float)x + 0.5f - (sx + w * 0.5f), py = (float)y + 0.5f - (sy + h * 0.5f);
    const float qx = fabsf(px) - w * 0.5f + radius, qy = fabsf(py) - h * 0.5f + radius;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - radius;
    const float c = 0.5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

static XsdkTexture shape_tex;
static uint8_t *shape_memory;
enum { SHAPE_W = 256, SHAPE_H = 128 };

/* Draws one white shape over black, reads it back and counts pixels more than 3 off what the shader should give. */
static void check_shape(const char *name, float sx, float sy, float w, float h, float radius)
{
    xsdk_gpu_set_target(SHAPE_W, SHAPE_H);
    xsdk_gpu_resolve_texture(&shape_tex, 0xFF000000);   /* clears the target to black */
    xsdk_gpu_set_target(SHAPE_W, SHAPE_H);
    gfx_frame_begin(SHAPE_W, SHAPE_H);
    gfx_rect(sx, sy, w, h, radius, 0xFFFFFFFF);
    xsdk_gpu_resolve_texture(&shape_tex, 0xFF000000);
    gfx_frame_end();
    xsdk_gpu_wait(xsdk_gpu_fence(), 1000);
    for (uintptr_t a = (uintptr_t)shape_memory; a < (uintptr_t)shape_memory + shape_tex.size_bytes; a += 128)
        __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
    static uint8_t rows[SHAPE_H][SHAPE_W * 4];
    xsdk_texture_download_rows(&shape_tex, 0, 0, SHAPE_H, rows, SHAPE_W * 4);
    int bad = 0, shown = 0;
    char where[200] = "";
    for (int y = 0; y < SHAPE_H; y++)
        for (int x = 0; x < SHAPE_W; x++) {
            /* White over black: red, green and blue are the coverage, alpha stays 255; the smallest byte, whatever
             * the byte order. */
            int got = 255;
            for (int k = 0; k < 4; k++)
                if (rows[y][x * 4 + k] < got)
                    got = rows[y][x * 4 + k];
            const int want = (int)(expected_coverage(x, y, sx, sy, w, h, radius) * 255.0f + 0.5f);
            if (abs(got - want) > 3) {
                bad++;
                if (shown++ < 4) {
                    char one[48];
                    snprintf(one, sizeof one, " (%d,%d) %d not %d", x, y, got, want);
                    strncat(where, one, sizeof where - strlen(where) - 1);
                }
            }
        }
    printf("shape check, %s: %d of %d pixels wrong%s\n", name, bad, SHAPE_W * SHAPE_H, where);
    /* The middle row, coarsely: what the shader drew across it. */
    printf("  middle row:");
    for (int x = 0; x < SHAPE_W; x += 8) {
        int got = 255;
        for (int k = 0; k < 4; k++)
            if (rows[SHAPE_H / 2][x * 4 + k] < got)
                got = rows[SHAPE_H / 2][x * 4 + k];
        printf(" %d", got);
    }
    printf("\n");
    fflush(stdout);
}

static void check_shapes(void)
{
    if (!NT_SUCCESS(xsdk_texture_init(&shape_tex, XSDK_FORMAT_R8G8B8A8, SHAPE_W, SHAPE_H, 1, true)))
        return;
    xsdk_texture_base_at_corner(&shape_tex);
    shape_memory = MmAllocatePhysicalMemoryEx(0, (shape_tex.size_bytes + 0xFFFF) & ~0xFFFFu,
                                              PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 4096);
    if (!shape_memory || !gfx_init())
        return;
    xsdk_texture_attach(&shape_tex, (volatile uint32_t *)shape_memory);
    progress("shape check");
    check_shape("flat rectangle over the whole target", 0, 0, SHAPE_W, SHAPE_H, 0);
    check_shape("rounded rectangle, radius 24", 32, 16, 192, 96, 24);
    check_shape("circle, radius 40", 88, 24, 80, 80, 40);
    gfx_shutdown();
}

static double per(uint64_t us, int frames)
{
    return frames ? us / 1000.0 / frames : 0;
}

int main(void)
{
    printf("hybench: the hybrid decoder (FFmpeg parses, the GPU predicts, FFmpeg replays) against plain decoding\n");
    printf("1080p, P frames, 6 slices; 60 fps needs 16.7 ms a frame\n\n");
    fflush(stdout);
    progress("GPU set-up");
    pthread_t dog;
    __real_pthread_create(&dog, NULL, watchdog, NULL);
    xsdk_thread_place(xsdk_pthread_handle(dog), 1);
    xsdk_thread_move(2);
    if (!NT_SUCCESS(xsdk_gpu_open()) || !gpumc_init(1920, 1088)) {
        printf("GPU set-up failed\n");
        devreturn();
        return 1;
    }
    screen_open = NT_SUCCESS(xsdk_text_open()) &&
                  NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &screen_w, &screen_h));
    progress("GPU ready");
    if (0) {
        printf("GPU set-up failed\n");
        devreturn();
        return 1;
    }
    check_shapes();
    char report[200];
    progress("calibration");
    const bool calibrated = gpumc_calibrate(report, sizeof report);
    printf("calibration: %s%s\n\n", report, calibrated ? "" : "  -- FAILED");
    fflush(stdout);
    start_replay_threads();
    if (calibrated) {
        for (unsigned c = 0; c < sizeof clips / sizeof clips[0] && gpu_usable; c++) {
            static uint32_t plain[MAX_FRAMES], hybrid[MAX_FRAMES];
            Totals a, b;
            printf("%s\n", clips[c].name);
            fflush(stdout);
            char clip_line[160];
            snprintf(clip_line, sizeof clip_line, "Clip %u: %s", c + 1, clips[c].name);
            progress(clip_line);
            const int n = decode(&clips[c], false, plain, &a);
            const int m = decode(&clips[c], true, hybrid, &b);
            int same = 0;
            for (int i = 0; i < n && i < m && i < MAX_FRAMES; i++)
                same += plain[i] == hybrid[i];
            printf("  plain : %6.2f ms a frame\n", per(a.total, a.frames));
            printf("  hybrid: %6.2f ms a frame = parse %.2f + grid out %.2f + GPU %.2f (luma %.2f, chroma %.2f) + untile "
                   "%.2f + replay %.2f + write-out %.2f\n",
                   per(b.total, b.frames), per(b.parse, b.frames), per(b.flush, b.frames), per(b.gpu, b.frames),
                   per(b.luma, b.gpu_frames), per(b.gpu - b.luma, b.gpu_frames), per(b.detile, b.frames),
                   per(b.replay, b.frames), per(b.writeout, b.frames));
            printf("  luma passes: G %.2f, B %.2f, H %.2f, J %.2f, final %.2f ms\n", per(b.pass[0], b.gpu_frames),
                   per(b.pass[1], b.gpu_frames), per(b.pass[2], b.gpu_frames), per(b.pass[3], b.gpu_frames),
                   per(b.pass[4], b.gpu_frames));
            printf("  frames identical: %d of %d%s\n\n", same, n, same == n && n == m ? "" : "  -- MISMATCH");
            fflush(stdout);
        }
    }
    if (gpu_hung) {
        /* devlaunch can't draw on a hung GPU (a black screen until a reboot): reboot, and it uploads this log. */
        printf("\nThe GPU hung: rebooting the console (devlaunch then uploads this log)\n");
        fflush(stdout);
        xsdk_thread_sleep(500);
        reboot_console();
        printf("the reboot call failed\n");
        fflush(stdout);
    }
    progress(gpu_hung ? "the GPU hung" : "done: back to devlaunch");
    progress("closing the GPU");
    if (screen_open)
        xsdk_present_close();
    gpumc_shutdown();
    xsdk_gpu_close();
    printf("hybench done\n");
    fflush(stdout);
    devreturn();
    return 0;
}
