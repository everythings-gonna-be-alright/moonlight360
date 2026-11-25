/*
 * decbench: how fast the console decodes 1080p streams shaped like Apollo/Sunshine's (High profile, P frames only,
 * 1 reference, 6 slices) at three bitrates, CABAC and CAVLC, deblocking on and skipped, each checked against FFmpeg
 * on the Mac. Also times copying a decoded frame into GPU textures. Runs headless, logs to decbench.log, and returns
 * to devlaunch when started from it.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/adler32.h>

#include "devreturn.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

extern const uint8_t cabac22_start[], cabac22_end[], cavlc22_start[], cavlc22_end[], cabac12_start[], cabac12_end[],
    cavlc12_start[], cavlc12_end[], cavlc5_start[], cavlc5_end[];
#ifdef DECBENCH_VMX
extern const uint8_t cavlc12s24_start[], cavlc12s24_end[], lean12s6_start[], lean12s6_end[], lean12s24_start[],
    lean12s24_end[], lean22s24_start[], lean22s24_end[], nvlean12s24_start[], nvlean12s24_end[], mpeg2_13_start[],
    mpeg2_13_end[], mpeg2_26_start[], mpeg2_26_end[], mid12s24_start[], mid12s24_end[], mid22s24_start[],
    mid22s24_end[], mpeg2_5_start[], mpeg2_5_end[];
#endif

/* The clips: one scene, 60 frames of 1080p30 with 6 slices, at three bitrates; each one's checksums decoded by FFmpeg
 * on the Mac (dev/bench/data, from dev/make_clips.sh). 1080p60 at 25 Mbit/s carries as many bits a frame as 1080p30
 * at 12.5, so the middle clips are a 1080p60 stream's frames.
 * Three checksums a clip, as deblocking has three ways to go: exact (edges between slices filtered too, which FFmpeg
 * does on one thread after the slices), fast (AV_CODEC_FLAG2_FAST, as the app decodes: each slice's thread filters
 * its own, edges between slices left alone), and skipped. */
typedef struct {
    const char *name;
    const uint8_t *start, *end;
    uint32_t checksum_exact, checksum_fast, checksum_skip;
    bool mpeg2;   /* an MPEG-2 elementary stream, not H.264 */
} Clip;
static const Clip clips[] = {
    { "CABAC 22.5 Mbps", cabac22_start, cabac22_end, 0xaf395430, 0x5bbf430a, 0xecfa8111 },
    { "CABAC 11.6 Mbps", cabac12_start, cabac12_end, 0x5e91c24b, 0xee883940, 0xf8b09897 },
    { "CAVLC 22.6 Mbps", cavlc22_start, cavlc22_end, 0x3aae36b1, 0x85c512ce, 0x736ed62b },
    { "CAVLC 11.5 Mbps", cavlc12_start, cavlc12_end, 0x4eebed01, 0x776728a2, 0xf22cd854 },
    { "CAVLC  5.4 Mbps", cavlc5_start, cavlc5_end, 0x6c9dec95, 0x22973151, 0xc5d3c8e0 },
#ifdef DECBENCH_VMX
    /* Streams that leave the console less to do (make_clips.sh). The 11.5 Mbit/s stream in 24 slices; then "lean"
     * streams, with no in-loop deblocking and whole-pixel motion: x264's ultrafast preset (Sunshine's software
     * encoder at "ultrafast"), and the 11.5 stream with only those two changes. A stream without deblocking
     * decodes to the same picture all three ways. */
    { "CAVLC 11.5, 24 slices", cavlc12s24_start, cavlc12s24_end, 0xddb90327, 0x347aa6d4, 0xf941aa8f },
    { "lean 11.4, 6 slices", lean12s6_start, lean12s6_end, 0x7ea19658, 0x7ea19658, 0x7ea19658 },
    { "lean 11.5, 24 slices", lean12s24_start, lean12s24_end, 0xb147096c, 0xb147096c, 0xb147096c },
    { "lean 22.6, 24 slices", lean22s24_start, lean22s24_end, 0x4387bf0a, 0x4387bf0a, 0x4387bf0a },
    { "nvlean 11.4, 24 slices", nvlean12s24_start, nvlean12s24_end, 0xbf867a24, 0xbf867a24, 0xbf867a24 },
    /* MPEG-2 as Sunshine's software encoder sends it (mpeg2_mode = 2): no deblocking, half-pixel motion, a slice
     * a macroblock row. */
    { "MPEG-2 13.4 Mbps", mpeg2_13_start, mpeg2_13_end, 0x34de5e07, 0x34de5e07, 0x34de5e07, true },
    { "MPEG-2 25.9 Mbps", mpeg2_26_start, mpeg2_26_end, 0xa85b4c17, 0xa85b4c17, 0xa85b4c17, true },
    /* "mid": x264's superfast preset with its fastdecode tune, 24 slices: no deblocking like the lean streams, but
     * sub-pixel motion and intra 4x4 and 8x8 blocks (Sunshine's software encoder at sw_preset = superfast,
     * sw_tune = fastdecode,zerolatency). */
    { "mid 11.4, 24 slices", mid12s24_start, mid12s24_end, 0x795e95e9, 0x795e95e9, 0x795e95e9 },
    { "mid 22.4, 24 slices", mid22s24_start, mid22s24_end, 0xcc3d2faa, 0xcc3d2faa, 0xcc3d2faa },
    /* MPEG-2 at the bits a frame of a 10 Mbit/s stream at 60 fps. */
    { "MPEG-2  5.6 Mbps", mpeg2_5_start, mpeg2_5_end, 0xf057eec0, 0xf057eec0, 0xf057eec0, true },
#endif
};

typedef struct {
    const char *name;
    int clip;
    int threads;
    bool skip_deblock;
    bool parse_only;   /* every macroblock parsed, none reconstructed: what reading the bitstream alone costs */
    bool no_mc;        /* everything but motion compensation: the difference is what a GPU MC pass takes over */
    uint32_t main_hw;
    uint32_t workers[6];
    int worker_count;
    bool chroma_c;     /* chroma prediction and deblocking in C, as before ppc/h264_xenon_vmx.h: the "before" timing */
    bool exact;        /* no AV_CODEC_FLAG2_FAST: edges between slices deblocked too, on one thread after the slices */
    int queue;         /* AVCodecContext.slices: with a frame's slice count, the threads take slices from one queue;
                          0 is FFmpeg's own way, a batch of as many slices as threads at a time */
    bool prof;         /* the decoder times its own parts (ff_xenon_prof), printed after the pass's line */
    bool large_pages;  /* malloc's big blocks (frames, the decoder's larger tables) on 64 KB pages, not 4 KB */
    int pitch, chroma_pitch;   /* pictures in physical memory with these row pitches, as the app lays them out for the
                                  GPU (2048 and 1024 at 1080p); 0: FFmpeg's own buffers, rows 1920 and 960 apart */
    int cr_offset;     /* with pitch: the third plane starts this many bytes into its memory */
    int tune;          /* ff_xenon_tune: the decoder's experiments, by bit (h264dec.h) */
} Pass;

/* All passes as the app decodes now: 6 slice threads on the three cores. */
#define ALL_HW 6, false, false, false, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_SKIP 6, true, false, false, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_NO_MC 6, true, false, true, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_PARSE 6, true, true, false, 2, { 3, 4, 5, 0, 1 }, 5
/* A clip's passes; with the AltiVec build, the first two again with chroma in C, for a before and after. */
#ifdef DECBENCH_VMX
#define CLIP(n) \
    { "deblock, chroma in C", n, ALL_HW, true }, { "deblock", n, ALL_HW }, \
    { "skip deblock, chroma in C", n, ALL_HW_SKIP, true }, { "skip deblock", n, ALL_HW_SKIP }, \
    { "no MC", n, ALL_HW_NO_MC }, { "PARSE ONLY", n, ALL_HW_PARSE }
#else
#define CLIP(n) \
    { "deblock", n, ALL_HW }, { "skip deblock", n, ALL_HW_SKIP }, { "no MC", n, ALL_HW_NO_MC }, \
    { "PARSE ONLY", n, ALL_HW_PARSE }
#endif
static const Pass passes[] = {
    CLIP(0), CLIP(1), CLIP(2), CLIP(3), CLIP(4),
#ifdef DECBENCH_VMX
    /* What deblocking costs when FFmpeg filters after the slices, on one thread (the app doesn't: it sets FAST). */
    { "deblock exact (1 thread)", 3, ALL_HW, .exact = true },
    /* 24 slices: six at a time as FFmpeg does it, then from a queue. */
    { "deblock, batches of 6", 5, ALL_HW }, { "deblock, queue", 5, ALL_HW, .queue = 24 },
    { "skip deblock, queue", 5, ALL_HW_SKIP, .queue = 24 },
    /* Lean streams: nothing skipped, the picture is the encoder's own. */
    { "all of it", 6, ALL_HW },
    { "all of it, queue", 7, ALL_HW, .queue = 24 }, { "no MC, queue", 7, ALL_HW_NO_MC, .queue = 24 },
    { "PARSE ONLY, queue", 7, ALL_HW_PARSE, .queue = 24 },
    { "all of it, queue", 8, ALL_HW, .queue = 24 },
    { "all of it, queue", 9, ALL_HW, .queue = 24 },
    /* MPEG-2: its 68 rows in 6 runs, one a thread, then in 24 runs from a queue. */
    { "all of it", 10, ALL_HW }, { "all of it, queue", 10, ALL_HW, .queue = 24 },
    { "all of it", 11, ALL_HW }, { "all of it, queue", 11, ALL_HW, .queue = 24 },
#endif
};

static uint64_t now_us(void);
static const void *kept_planes[3];   /* where the last frame's planes were, to see which memory they came from */

/* malloc's blocks of 256 KB and more come from here (xsdk's sbrk.c): on 4 KB pages, or 64 KB ones for a pass that
 * asks. The console's address translation holds few pages at once, so the page size decides how often it misses. */
void *__real___xsdk_heap_map(size_t size_bytes);
static bool large_pages;
static unsigned large_page_blocks, large_page_failures;

void *__wrap___xsdk_heap_map(size_t size_bytes)
{
    if (large_pages) {
        void *base = NULL;
        uint32_t bytes = ((uint32_t)size_bytes + 0xFFFF) & ~0xFFFFu;
        if (NT_SUCCESS(NtAllocateVirtualMemory(&base, &bytes, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                               PAGE_READWRITE, 0))) {
            large_page_blocks++;
            return base;
        }
        large_page_failures++;
    }
    return __real___xsdk_heap_map(size_bytes);
}

#ifdef DECBENCH_VMX
/* "tune" in the dev server's arguments runs these instead: the streams the app plays, with the decoder timing its
 * own parts, then with each of its experiments switched on. */
#define LEAN12 7
#define LEAN22 8
#define NVLEAN 9
#define NORMAL24 5
#define MID12 12
#define MID22 13
#define SIX ALL_HW, .queue = 24
#define ONE 1, false, false, false, 2, { 0 }, 0
#define AS_APP .pitch = 2048, .chroma_pitch = 1024
#define SPREAD .pitch = 2304, .chroma_pitch = 1280
/* The app's layout since 2026-11-25: rows 2304 and 1280 bytes apart, the third plane a page into its memory. */
#define APP_NOW .pitch = 2304, .chroma_pitch = 1280, .cr_offset = 4096
/* The decoder as the app runs it (ff_xenon_tune's default), against FFmpeg's stock paths in FFmpeg's own buffers. */
/* H.264's part of it: the MPEG-2 passes below switch MPEG-2's three on one after another. */
#define NOW 8 | 32 | 64 | 128 | 768
static const Pass tune_passes[] = {
    { "PROFILE, 1 thr, as the app", LEAN12, ONE, .prof = true, .tune = NOW, APP_NOW },
    { "PROFILE, 6 thr, as the app", LEAN12, SIX, .prof = true, .tune = NOW, APP_NOW },
    { "PROFILE, 6 thr, as the app", LEAN22, SIX, .prof = true, .tune = NOW, APP_NOW },
    { "PROFILE, 6 thr, as the app", MID12, SIX, .prof = true, .tune = NOW, APP_NOW },
    { "PROFILE, 6 thr, as the app", NORMAL24, SIX, .prof = true, .tune = NOW, APP_NOW },
    { "stock", LEAN12, SIX }, { "as the app", LEAN12, SIX, .tune = NOW, APP_NOW },
    { "stock", LEAN22, SIX }, { "as the app", LEAN22, SIX, .tune = NOW, APP_NOW },
    { "stock", NVLEAN, SIX }, { "as the app", NVLEAN, SIX, .tune = NOW, APP_NOW },
    { "stock", MID12, SIX }, { "as the app", MID12, SIX, .tune = NOW, APP_NOW },
    { "stock", MID22, SIX }, { "as the app", MID22, SIX, .tune = NOW, APP_NOW },
    { "stock", NORMAL24, SIX }, { "as the app", NORMAL24, SIX, .tune = NOW, APP_NOW },
    { "as the app, skip deblock", NORMAL24, ALL_HW_SKIP, .queue = 24, .tune = NOW, APP_NOW },
};

/* "tune mpeg2" runs these: MPEG-2 at three bitrates, with the decoder timing its own parts (libavcodec/xenon_mpeg.h)
 * and its steps of 2026-11-25 switched on one after another (ff_xenon_tune's bits 1024, 2048 and 4096). */
#define M5 14
#define M13 10
#define M26 11
#define COPIES 1024
#define SHORTCUT 2048   /* a block of one coefficient without the transform */
#define TRANSFORM 4096  /* the transform in vector floats */
#define MPEG2_CLIP(n) \
    { "PROFILE, 1 thr, vector copies", n, ONE, .prof = true, .tune = NOW | COPIES, APP_NOW }, \
    { "PROFILE, 1 thr, + shortcut", n, ONE, .prof = true, .tune = NOW | COPIES | SHORTCUT, APP_NOW }, \
    { "PROFILE, 1 thr, + transform", n, ONE, .prof = true, .tune = NOW | COPIES | TRANSFORM, APP_NOW }, \
    { "PROFILE, 1 thr, + both", n, ONE, .prof = true, .tune = NOW | COPIES | SHORTCUT | TRANSFORM, APP_NOW }, \
    { "C copies (before)", n, SIX, .tune = NOW, APP_NOW }, \
    { "vector copies", n, SIX, .tune = NOW | COPIES, APP_NOW }, \
    { "+ shortcut", n, SIX, .tune = NOW | COPIES | SHORTCUT, APP_NOW }, \
    { "+ transform", n, SIX, .tune = NOW | COPIES | TRANSFORM, APP_NOW }, \
    { "+ both", n, SIX, .tune = NOW | COPIES | SHORTCUT | TRANSFORM, APP_NOW }, \
    { "PROFILE, 6 thr, + both", n, SIX, .prof = true, .tune = NOW | COPIES | SHORTCUT | TRANSFORM, APP_NOW }
static const Pass mpeg2_passes[] = { MPEG2_CLIP(M5), MPEG2_CLIP(M13), MPEG2_CLIP(M26) };

/* What memory alone costs a frame of whole-pixel motion compensation: every macroblock of a 1080p frame copied from
 * one picture to another as the decoder does it (16 rows of 16 luma bytes, 8 rows of 8 for each chroma plane, in
 * raster order), with the pictures in memory of each kind, with and without prefetching; and the same copy on one
 * macroblock over and over, which is the code's cost without the memory's. */
enum { MT_W = 1920, MT_H = 1088, MT_LUMA = MT_W * MT_H, MT_BYTES = MT_LUMA * 3 / 2, MT_ROUNDS = 12 };

static __attribute__((noinline)) void copy_frame(uint8_t *dst, const uint8_t *src, int prefetch, bool hot)
{
    for (int my = 0; my < MT_H / 16; my++)
        for (int mx = 0; mx < MT_W / 16; mx++) {
            const int x = hot ? 0 : mx * 16, y = hot ? 0 : my * 16;
            const uint32_t *s = (const uint32_t *)(src + y * MT_W + x);
            uint32_t *d = (uint32_t *)(dst + y * MT_W + x);
            if (prefetch && !(mx & 7))   /* a cache line is 128 bytes: 8 macroblocks' worth of each row */
                for (int r = 0; r < 16; r++) {
                    __asm__ volatile("dcbt 0,%0" : : "r"((const uint8_t *)s + r * MT_W + prefetch));
                    __asm__ volatile("dcbtst 0,%0" : : "r"((const uint8_t *)d + r * MT_W + prefetch));
                }
            for (int r = 0; r < 16; r++, s += MT_W / 4, d += MT_W / 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
            }
            for (int plane = 0; plane < 2; plane++) {
                const size_t at = MT_LUMA + plane * (MT_LUMA / 4) + (y / 2) * (MT_W / 2) + x / 2;
                const uint32_t *cs = (const uint32_t *)(src + at);
                uint32_t *cd = (uint32_t *)(dst + at);
                if (prefetch && !(mx & 15))
                    for (int r = 0; r < 8; r++) {
                        __asm__ volatile("dcbt 0,%0" : : "r"((const uint8_t *)cs + r * (MT_W / 2) + prefetch));
                        __asm__ volatile("dcbtst 0,%0" : : "r"((const uint8_t *)cd + r * (MT_W / 2) + prefetch));
                    }
                for (int r = 0; r < 8; r++, cs += MT_W / 8, cd += MT_W / 8) {
                    cd[0] = cs[0];
                    cd[1] = cs[1];
                }
            }
        }
}

static void memory_test(const char *name, uint8_t *a, uint8_t *b)
{
    if (!a || !b) {
        printf("  %-34s not available\n", name);
        return;
    }
    memset(a, 1, MT_BYTES);
    memset(b, 2, MT_BYTES);
    static const struct {
        const char *name;
        int prefetch;
        bool hot;
    } ways[] = { { "one MB over and over", 0, true }, { "frame", 0, false }, { "frame, prefetch 128 ahead", 128, false },
                 { "frame, prefetch 256 ahead", 256, false } };
    printf("  %-34s at %p:", name, (void *)a);
    for (unsigned w = 0; w < sizeof ways / sizeof ways[0]; w++) {
        const uint64_t t0 = now_us();
        for (int round = 0; round < MT_ROUNDS; round++)   /* each frame is copied from the last one written */
            copy_frame(round & 1 ? a : b, round & 1 ? b : a, ways[w].prefetch, ways[w].hot);
        const double cycles = (double)(now_us() - t0) * 3200.0 / (MT_ROUNDS * (MT_W / 16) * (MT_H / 16));
        printf("  %s %.0f", ways[w].name, cycles);
    }
    printf("  (cycles a macroblock)\n");
    fflush(stdout);
}

static void memory_tests(void)
{
    xsdk_thread_move(2);
    printf("Copying a 1080p frame macroblock by macroblock, by the memory the pictures are in:\n");
    uint8_t *heap_a = malloc(MT_BYTES), *heap_b = malloc(MT_BYTES);
    memory_test("malloc (4 KB pages)", heap_a, heap_b);
    free(heap_a);
    free(heap_b);
    void *va = NULL, *vb = NULL;
    uint32_t bytes_a = (MT_BYTES + 0xFFFF) & ~0xFFFFu, bytes_b = bytes_a;
    if (!NT_SUCCESS(NtAllocateVirtualMemory(&va, &bytes_a, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE, 0)))
        va = NULL;
    if (!NT_SUCCESS(NtAllocateVirtualMemory(&vb, &bytes_b, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE, 0)))
        vb = NULL;
    memory_test("virtual, 64 KB pages", va, vb);
    uint8_t *pa = MmAllocatePhysicalMemoryEx(0, bytes_a, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0x10000);
    uint8_t *pb = MmAllocatePhysicalMemoryEx(0, bytes_a, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 0x10000);
    memory_test("physical, 64 KB pages (as the app)", pa, pb);
    /* 16 MB pages: one block holds both pictures. */
    uint8_t *big = MmAllocatePhysicalMemoryEx(0, 0x1000000, PAGE_READWRITE | 0x80000000u, 0, UINT32_MAX, 0x1000000);
    memory_test("physical, one 16 MB page", big, big ? big + 0x800000 : NULL);
    printf("\n");
}

/* The decoder's own timing (h264dec.h, the same order). */
enum { XPROF_SKIP, XPROF_HEAD, XPROF_RESIDUAL, XPROF_MC, XPROF_INTRA, XPROF_IDCT, XPROF_FILTER, XPROF_TOTAL,
       XPROF_START, XPROF_END, XPROF_MBS, XPROF_SKIPPED, XPROF_INTRA_MBS, XPROF_SLICES, XPROF_MID,
       XPROF_MC_LUMA, XPROF_MC_CHROMA, XPROF_MC_PREFETCH, XPROF_IDCT_LUMA, XPROF_FAST_MBS, XPROF_SLICE_N = 32 };
enum { XPROF_MAIN_FRAME, XPROF_MAIN_SPLIT, XPROF_MAIN_NALS, XPROF_MAIN_EXECUTE, XPROF_MAIN_FRAMES, XPROF_MAIN_N = 8 };
extern int ff_xenon_prof, ff_xenon_tune;
extern uint64_t ff_xenon_prof_slice[64][XPROF_SLICE_N];
extern uint64_t ff_xenon_prof_main[XPROF_MAIN_N];
static double ticks_a_ms;   /* the timebase's rate, measured in main */

/* libavcodec/xenon_mpeg.h */
enum { XMPROF_PARSE, XMPROF_MC, XMPROF_IDCT, XMPROF_TOTAL, XMPROF_MID, XMPROF_MBS, XMPROF_SKIPPED, XMPROF_INTRA,
       XMPROF_NO_RESIDUAL, XMPROF_ZERO_MV, XMPROF_HPEL, XMPROF_BLOCKS = XMPROF_HPEL + 4, XMPROF_BLOCK_KIND,
       XMPROF_N = 32 };
extern uint64_t ff_xenon_mpeg_prof[128][XMPROF_N];

static void print_mpeg2_profile(int frames_decoded, double ms_a_frame)
{
    double t[XMPROF_N] = { 0 };
    const double frames = frames_decoded > 0 ? frames_decoded : 1;
    for (int row = 0; row < 128; row++)
        for (int k = 0; k < XMPROF_N; k++)
            t[k] += (double)ff_xenon_mpeg_prof[row][k] / frames;
    const double ms = 1.0 / ticks_a_ms, cyc = 3200000.0 / ticks_a_ms;
    const double coded = t[XMPROF_MBS] - t[XMPROF_SKIPPED], inter = t[XMPROF_MBS] - t[XMPROF_INTRA];
    printf("    slices, ms of thread time a frame: %.2f = reading the bitstream %.2f + motion %.2f + residual %.2f + "
           "the rest %.2f; %.2f threads' worth of the frame's time\n",
           t[XMPROF_TOTAL] * ms, t[XMPROF_PARSE] * ms, t[XMPROF_MC] * ms, t[XMPROF_IDCT] * ms,
           (t[XMPROF_TOTAL] - t[XMPROF_PARSE] - t[XMPROF_MC] - t[XMPROF_IDCT]) * ms,
           ms_a_frame > 0 ? t[XMPROF_TOTAL] * ms / ms_a_frame : 0);
    printf("    a frame's %.0f MBs: %.0f skipped, %.0f intra, %.0f coded inter (%.0f of them with no residual); %.0f "
           "not moved; whole-pixel %.0f, half across %.0f, half down %.0f, both %.0f\n",
           t[XMPROF_MBS], t[XMPROF_SKIPPED], t[XMPROF_INTRA], coded - t[XMPROF_INTRA],
           t[XMPROF_NO_RESIDUAL] - t[XMPROF_SKIPPED], t[XMPROF_ZERO_MV], t[XMPROF_HPEL], t[XMPROF_HPEL + 1],
           t[XMPROF_HPEL + 2], t[XMPROF_HPEL + 3]);
    printf("    a frame's %.0f blocks: %.0f DC alone, %.0f DC and the mismatch bit, %.0f reaching up to 3 coefficients, "
           "%.0f up to 10, %.0f up to 21, %.0f further\n",
           t[XMPROF_BLOCKS], t[XMPROF_BLOCK_KIND], t[XMPROF_BLOCK_KIND + 1], t[XMPROF_BLOCK_KIND + 2],
           t[XMPROF_BLOCK_KIND + 3], t[XMPROF_BLOCK_KIND + 4], t[XMPROF_BLOCK_KIND + 5]);
    printf("    cycles: bitstream %.0f a coded MB; motion %.0f an inter MB; residual %.0f a block\n",
           t[XMPROF_PARSE] * cyc / (coded > 0 ? coded : 1), t[XMPROF_MC] * cyc / (inter > 0 ? inter : 1),
           t[XMPROF_IDCT] * cyc / (t[XMPROF_BLOCKS] > 0 ? t[XMPROF_BLOCKS] : 1));
}

static void print_profile(void)
{
    const uint64_t *m = ff_xenon_prof_main;
    const double frames = m[XPROF_MAIN_FRAMES] ? (double)m[XPROF_MAIN_FRAMES] : 1, f = 1.0 / (ticks_a_ms * frames);
    uint64_t s[XPROF_SLICE_N] = { 0 };
    int used = 0;
    for (int i = 0; i < 64; i++) {
        for (int k = 0; k < XPROF_SLICE_N; k++)
            s[k] += ff_xenon_prof_slice[i][k];
        if (ff_xenon_prof_slice[i][XPROF_SLICES])
            used = i + 1;
    }
    const uint64_t parts = s[XPROF_SKIP] + s[XPROF_HEAD] + s[XPROF_RESIDUAL] + s[XPROF_MC] + s[XPROF_INTRA] +
                           s[XPROF_IDCT] + s[XPROF_FILTER];
    printf("    feeding thread, ms a frame: %.2f in the decoder = %.2f splitting the packet + %.2f headers and frame "
           "start + %.2f slices (handed out, awaited) + %.2f after\n",
           m[XPROF_MAIN_FRAME] * f, m[XPROF_MAIN_SPLIT] * f,
           (m[XPROF_MAIN_NALS] - m[XPROF_MAIN_SPLIT] - m[XPROF_MAIN_EXECUTE]) * f, m[XPROF_MAIN_EXECUTE] * f,
           (m[XPROF_MAIN_FRAME] - m[XPROF_MAIN_NALS]) * f);
    printf("    slices, ms of thread time a frame: %.2f = skipped MBs %.2f + MB headers %.2f + coefficients %.2f + "
           "motion %.2f + intra prediction %.2f + residual added %.2f + deblocking %.2f + the rest %.2f; "
           "%.2f threads busy on average\n",
           s[XPROF_TOTAL] * f, s[XPROF_SKIP] * f, s[XPROF_HEAD] * f, s[XPROF_RESIDUAL] * f, s[XPROF_MC] * f,
           s[XPROF_INTRA] * f, s[XPROF_IDCT] * f, s[XPROF_FILTER] * f, (s[XPROF_TOTAL] - parts) * f,
           m[XPROF_MAIN_EXECUTE] ? (double)s[XPROF_TOTAL] / (double)m[XPROF_MAIN_EXECUTE] : 0);
    const double mbs = s[XPROF_MBS] ? (double)s[XPROF_MBS] : 1, cyc = 3200000.0 / ticks_a_ms;
    const double coded = mbs - (double)s[XPROF_SKIPPED] > 0 ? mbs - (double)s[XPROF_SKIPPED] : 1;
    const double intra = s[XPROF_INTRA_MBS] ? (double)s[XPROF_INTRA_MBS] : 1;
    const double inter = mbs - (double)s[XPROF_INTRA_MBS] > 0 ? mbs - (double)s[XPROF_INTRA_MBS] : 1;
    printf("    a frame's %.0f MBs: %.0f skipped, %.0f intra. Cycles each: skipped MB %.0f, header %.0f, coefficients "
           "%.0f (coded MBs); motion %.0f (inter MBs); intra prediction %.0f (intra MBs); residual added %.0f; "
           "deblocking %.0f (all MBs)\n",
           mbs / frames, (double)s[XPROF_SKIPPED] / frames, (double)s[XPROF_INTRA_MBS] / frames,
           s[XPROF_SKIPPED] ? s[XPROF_SKIP] * cyc / (double)s[XPROF_SKIPPED] : 0, s[XPROF_HEAD] * cyc / coded,
           s[XPROF_RESIDUAL] * cyc / coded, s[XPROF_MC] * cyc / inter, s[XPROF_INTRA] * cyc / intra,
           s[XPROF_IDCT] * cyc / mbs, s[XPROF_FILTER] * cyc / mbs);
    printf("    motion, cycles an inter MB: %.0f = luma calls %.0f + chroma calls %.0f + FFmpeg's prefetching %.0f + "
           "the rest %.0f; %.0f MBs a frame took the whole-pixel path. Residual added, cycles a MB: luma %.0f, "
           "chroma %.0f\n",
           s[XPROF_MC] * cyc / inter, s[XPROF_MC_LUMA] * cyc / inter, s[XPROF_MC_CHROMA] * cyc / inter,
           s[XPROF_MC_PREFETCH] * cyc / inter,
           (double)(s[XPROF_MC] - s[XPROF_MC_LUMA] - s[XPROF_MC_CHROMA] - s[XPROF_MC_PREFETCH]) * cyc / inter,
           (double)s[XPROF_FAST_MBS] / frames, s[XPROF_IDCT_LUMA] * cyc / mbs,
           (double)(s[XPROF_IDCT] - s[XPROF_IDCT_LUMA]) * cyc / mbs);
    if (used > 1)
        return;
    printf("    slices' start > end after being handed out, ms:");
    for (int i = 0; i < used; i++) {
        const uint64_t *p = ff_xenon_prof_slice[i];
        const double n = p[XPROF_SLICES] ? (double)p[XPROF_SLICES] * ticks_a_ms : 1;
        printf(" %.1f>%.1f", p[XPROF_START] / n, p[XPROF_END] / n);
    }
    printf("\n");
}
#endif

extern void (*ff_h264_mb_hook)(const void *h, void *sl);
extern int ff_h264_skip_mc;
extern int ff_xenon_chroma_c;
static void parse_only_hook(const void *h, void *sl)
{
    (void)h, (void)sl;
}

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

/* FFmpeg's threads come from pthread_create: each goes on the next hardware thread of the pass's list. */
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
static const uint32_t *worker_hw;
static int worker_count;
static atomic_int next_worker;

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    int ret = __real_pthread_create(thread, attr, start, arg);
    if (ret == 0 && worker_count) {
        uint32_t hw = worker_hw[atomic_fetch_add(&next_worker, 1) % worker_count];
        xsdk_thread_place(xsdk_pthread_handle(*thread), hw);
    }
    return ret;
}

static AVFrame *kept;   /* one decoded frame, for the upload test */

/* Pictures laid out as the app's (video.c): each plane in physical memory on 64 KB pages, its rows a chosen pitch
 * apart. The data cache has 64 sets of four 128-byte lines, so rows 8192 bytes apart share a set: at the app's
 * 1080p pitch of 2048 a macroblock's 16 rows fall into 4 sets, in the picture being written and the one being read
 * alike. */
static int buffer_pitch[3], buffer_cr_offset;
static AVBufferPool *buffer_pools[3];

static void buffer_free(void *opaque, uint8_t *data)
{
    MmFreePhysicalMemory(0, data - (intptr_t)opaque);
}

static AVBufferRef *buffer_alloc(void *opaque, size_t size)
{
    const intptr_t offset = (intptr_t)opaque;
    const uint32_t rounded = ((uint32_t)size + (uint32_t)offset + 0xFFFF) & ~0xFFFFu;
    uint8_t *memory = MmAllocatePhysicalMemoryEx(0, rounded, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 4096);
    if (!memory)
        return NULL;
    AVBufferRef *buffer = av_buffer_create(memory + offset, size, buffer_free, opaque, 0);
    if (!buffer)
        MmFreePhysicalMemory(0, memory);
    return buffer;
}

static int get_buffer(AVCodecContext *c, AVFrame *f, int flags)
{
    if (!buffer_pitch[0] || (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P))
        return avcodec_default_get_buffer2(c, f, flags);
    int w = f->width, h = f->height, align[AV_NUM_DATA_POINTERS];
    avcodec_align_dimensions2(c, &w, &h, align);
    for (int p = 0; p < 3; p++) {
        if (!buffer_pools[p])
            buffer_pools[p] = av_buffer_pool_init2((size_t)buffer_pitch[p] * (size_t)(p ? (h + 1) / 2 : h),
                                                   (void *)(intptr_t)(p == 2 ? buffer_cr_offset : 0), buffer_alloc, NULL);
        f->buf[p] = buffer_pools[p] ? av_buffer_pool_get(buffer_pools[p]) : NULL;
        if (!f->buf[p]) {
            for (int q = 0; q < p; q++)
                av_buffer_unref(&f->buf[q]);
            return AVERROR(ENOMEM);
        }
        f->data[p] = f->buf[p]->data;
        f->linesize[p] = buffer_pitch[p];
    }
    f->extended_data = f->data;
    return 0;
}

/* Splits the Annex B stream into access units at each first-slice NAL (first_mb_in_slice == 0). */
static const uint8_t *next_access_unit(const uint8_t *p, const uint8_t *end, const uint8_t **au_end)
{
    const uint8_t *start = p;
    bool seen_slice = false;
    while (p + 4 < end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            const uint8_t type = p[3] & 0x1F;
            const bool slice = type == 1 || type == 5;
            const bool first = slice && (p[4] & 0x80);   /* ue(v) 0 is a single 1 bit */
            const bool param = type == 7 || type == 8 || type == 9;
            if ((first || (param && seen_slice)) && seen_slice) {
                const uint8_t *cut = p > start && p[-1] == 0 ? p - 1 : p;
                *au_end = cut;
                return start;
            }
            if (slice)
                seen_slice = true;
            p += 3;
        } else {
            p++;
        }
    }
    *au_end = end;
    return start;
}

/* MPEG-2: a picture's data runs from its sequence, GOP or picture header (start codes B3, B8, 00: whichever comes
 * first after the last picture's slices) to the next picture's. */
static const uint8_t *next_mpeg2_picture(const uint8_t *p, const uint8_t *end, const uint8_t **au_end)
{
    const uint8_t *start = p;
    bool seen_picture = false;
    for (; p + 3 < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            const uint8_t code = p[3];
            if ((code == 0xB3 || code == 0xB8 || code == 0x00) && seen_picture) {
                *au_end = p;
                return start;
            }
            if (code == 0x00)
                seen_picture = true;
            p += 3;
        }
    }
    *au_end = end;
    return start;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void run(const Pass *pass)
{
    const Clip *clip = &clips[pass->clip];
    const uint8_t *data = clip->start;
    const uint8_t *end = clip->end;
    xsdk_thread_move(pass->main_hw);
    worker_hw = pass->workers;
    worker_count = pass->worker_count;
    atomic_store(&next_worker, 0);

    const AVCodec *codec = avcodec_find_decoder(clip->mpeg2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!pass->exact)
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->thread_count = pass->threads;
    ctx->slices = pass->queue;
    if (pass->skip_deblock)
        ctx->skip_loop_filter = AVDISCARD_ALL;
    buffer_pitch[0] = pass->pitch;
    buffer_pitch[1] = buffer_pitch[2] = pass->chroma_pitch;
    buffer_cr_offset = pass->cr_offset;
    ctx->get_buffer2 = get_buffer;
    ff_h264_mb_hook = pass->parse_only ? parse_only_hook : NULL;
    ff_h264_skip_mc = pass->no_mc;
    ff_xenon_chroma_c = pass->chroma_c;
#ifdef DECBENCH_VMX
    ff_xenon_tune = pass->tune;
    large_pages = pass->large_pages;
    large_page_blocks = large_page_failures = 0;
    memset(ff_xenon_prof_slice, 0, sizeof ff_xenon_prof_slice);
    memset(ff_xenon_prof_main, 0, sizeof ff_xenon_prof_main);
    memset(ff_xenon_mpeg_prof, 0, sizeof ff_xenon_mpeg_prof);
    ff_xenon_prof = pass->prof;
#endif
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        printf("%-40s  avcodec_open2 failed\n", pass->name);
        return;
    }
    worker_count = 0;

    enum { MAX = 256 };
    static uint32_t gaps[MAX];
    int frames = 0, gap_count = 0;
    uint32_t sum = 1;
    uint64_t checksum_us = 0;
    uint64_t first_us = 0, last_us = 0, busy_us = 0;
    const uint8_t *p = data;
    bool flushing = false;
    for (;;) {
        if (p < end) {
            const uint8_t *au_end;
            const uint8_t *au = clip->mpeg2 ? next_mpeg2_picture(p, end, &au_end) : next_access_unit(p, end, &au_end);
            p = au_end;
            pkt->data = (uint8_t *)au;
            pkt->size = (int)(au_end - au);
        } else if (!flushing) {
            flushing = true;
        } else {
            break;
        }
        uint64_t t0 = now_us();
        if (!first_us)
            first_us = t0;
        int ret = avcodec_send_packet(ctx, flushing ? NULL : pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            printf("  send_packet error %d\n", ret);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            /* Times leave out the checksums: the clock is moved back by the time they took. */
            uint64_t t = now_us() - checksum_us;
            if (last_us && gap_count < MAX)
                gaps[gap_count++] = (uint32_t)(t - last_us);
            last_us = t;
            frames++;
            const uint64_t c0 = now_us();
            for (int pl = 0; pl < 3; pl++) {
                const int w = pl ? (frame->width + 1) / 2 : frame->width;
                const int h = pl ? (frame->height + 1) / 2 : frame->height;
                for (int y = 0; y < h; y++)
                    sum = av_adler32_update(sum, frame->data[pl] + y * frame->linesize[pl], w);
            }
            checksum_us += now_us() - c0;
            if (!kept) {
                kept = av_frame_clone(frame);
            }
            for (int pl = 0; pl < 3; pl++)
                kept_planes[pl] = frame->data[pl];
            av_frame_unref(frame);
        }
        busy_us += now_us() - t0;
    }
    qsort(gaps, gap_count, sizeof gaps[0], cmp_u32);
    const double avg = frames ? (last_us - first_us) / 1000.0 / frames : 0;
    const uint32_t expected = pass->skip_deblock ? clip->checksum_skip
                              : pass->exact      ? clip->checksum_exact
                                                 : clip->checksum_fast;
    printf("%-22s %-26s avg %6.2f ms (%5.1f fps)  p95 %6.2f ms  %08x %s\n", clip->name, pass->name, avg,
           avg > 0 ? 1000.0 / avg : 0, gap_count ? gaps[gap_count * 95 / 100] / 1000.0 : 0, (unsigned)sum,
           pass->parse_only ? "(no picture: parsing alone)"
           : pass->no_mc   ? "(no picture: no motion compensation)"
           : sum == expected ? "matches the Mac"
                             : "MISMATCH");
    ff_h264_mb_hook = NULL;
    ff_h264_skip_mc = 0;
    ff_xenon_chroma_c = 0;
#ifdef DECBENCH_VMX
    if (pass->large_pages)
        printf("    %u blocks on 64 KB pages, %u refused; a frame's planes at %p %p %p\n", large_page_blocks,
               large_page_failures, kept_planes[0], kept_planes[1], kept_planes[2]);
    if (pass->prof && clip->mpeg2)
        print_mpeg2_profile(frames, avg);
    else if (pass->prof)
        print_profile();
    ff_xenon_prof = ff_xenon_tune = 0;
    large_pages = false;
#endif
    fflush(stdout);
    (void)busy_us;
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    if (kept && pass->pitch)
        av_frame_free(&kept);   /* the upload test wants a frame from FFmpeg's own buffers */
    for (int pl = 0; pl < 3; pl++)
        av_buffer_pool_uninit(&buffer_pools[pl]);
    buffer_pitch[0] = 0;
}

/* Copying one decoded frame's three planes into GPU textures, as the app does each frame. */
static void upload_test(bool tiled)
{
    if (!kept)
        return;
    XsdkTexture planes[3];
    for (int i = 0; i < 3; i++) {
        uint32_t w = i ? (uint32_t)(kept->width + 1) / 2 : (uint32_t)kept->width;
        uint32_t h = i ? (uint32_t)(kept->height + 1) / 2 : (uint32_t)kept->height;
        if (!NT_SUCCESS(xsdk_texture_create(&planes[i], XSDK_FORMAT_R8, w, h, 1, tiled))) {
            printf("upload: texture create failed\n");
            return;
        }
    }
    enum { ROUNDS = 30 };
    uint64_t t0 = now_us();
    for (int r = 0; r < ROUNDS; r++)
        for (int i = 0; i < 3; i++)
            xsdk_texture_upload(&planes[i], 0, kept->data[i], (uint32_t)kept->linesize[i]);
    const double ms = (now_us() - t0) / 1000.0 / ROUNDS;
    printf("upload %dx%d YUV into %s textures: %.2f ms a frame (on one hardware thread)\n", kept->width,
           kept->height, tiled ? "tiled " : "linear", ms);
    for (int i = 0; i < 3; i++)
        xsdk_texture_free(&planes[i]);
}

#ifdef DECBENCH_VMX
/* What single instructions cost on this CPU, to tell which of the compiler's choices in the scalar decoding code
 * (bit reading, motion vector maths) are worth writing around. IBM's documents for this core family say shifts by a
 * register and sign-extending loads run from microcode, and a load right after a store to the same address waits
 * for the store; each loop here differs from its neighbour by that one instruction. */
#define COST_LOOP(fn, body) \
    static __attribute__((noinline)) uint32_t fn(uint32_t n, uint32_t *mem, uint32_t k) \
    { \
        uint32_t acc = 1, t = 0; \
        __asm__ volatile("mtctr %[n]\n1:\n" body "\nadd %[acc],%[acc],%[t]\nbdnz 1b" \
                         : [acc] "+b"(acc), [t] "+b"(t) \
                         : [n] "b"(n), [mem] "b"(mem), [k] "b"(k) \
                         : "ctr", "cr0", "xer", "memory", "lr", "v0", "v1", "v2"); \
        return acc; \
    }
COST_LOOP(cost_add, "addi %[t],%[acc],3")
COST_LOOP(cost_shift_const, "slwi %[t],%[acc],3")
COST_LOOP(cost_shift_var, "slw %[t],%[acc],%[k]")
COST_LOOP(cost_sraw_const, "srawi %[t],%[acc],3")
COST_LOOP(cost_sraw_var, "sraw %[t],%[acc],%[k]")
COST_LOOP(cost_rotate_var, "rlwnm %[t],%[acc],%[k],0,31")
COST_LOOP(cost_shift_record, "rlwinm. %[t],%[acc],3,0,28")
COST_LOOP(cost_load_extend, "lhz %[t],0(%[mem])\nextsh %[t],%[t]")
COST_LOOP(cost_load_lha, "lha %[t],0(%[mem])")
COST_LOOP(cost_store_load_apart, "stw %[acc],0(%[mem])\nlwz %[t],64(%[mem])")
COST_LOOP(cost_store_load_same, "stw %[acc],0(%[mem])\nlwz %[t],0(%[mem])")
COST_LOOP(cost_multiply, "mullw %[t],%[acc],%[k]")
/* The vector code's instructions: loads and stores, the shuffles around an unaligned load, a store of one word of
 * a vector, and the cache hints. */
COST_LOOP(cost_lvx, "lvx 0,0,%[mem]")
COST_LOOP(cost_stvx, "stvx 0,0,%[mem]")
COST_LOOP(cost_stvx_lvx_same, "stvx 0,0,%[mem]\nlvx 1,0,%[mem]")
COST_LOOP(cost_stw_lvx_same, "stw %[acc],0(%[mem])\nlvx 1,0,%[mem]")
COST_LOOP(cost_lvsl, "lvsl 0,0,%[mem]")
COST_LOOP(cost_vperm, "vperm 0,0,1,2")
COST_LOOP(cost_vaddubm, "vaddubm 0,0,1")
COST_LOOP(cost_stvewx, "stvewx 0,0,%[mem]")
COST_LOOP(cost_dcbt, "dcbt 0,%[mem]")
COST_LOOP(cost_dcbtst, "dcbtst 0,%[mem]")
COST_LOOP(cost_lwz, "lwz %[t],0(%[mem])")
COST_LOOP(cost_lwz_unaligned, "lwz %[t],3(%[mem])")
COST_LOOP(cost_stw, "stw %[acc],0(%[mem])")
COST_LOOP(cost_lbz, "lbz %[t],5(%[mem])")
COST_LOOP(cost_bctrl, "bl 2f\nb 3f\n2:\nblr\n3:")
COST_LOOP(cost_lhz_extsh, "lhz %[t],0(%[mem])\nextsh %[t],%[t]")
COST_LOOP(cost_andi_record, "andi. %[t],%[acc],255")
COST_LOOP(cost_extsb, "extsb %[t],%[acc]")
COST_LOOP(cost_cntlzw, "cntlzw %[t],%[acc]")
COST_LOOP(cost_mfcr, "mfcr %[t]")
COST_LOOP(cost_lwzx, "lwzx %[t],%[mem],%[k]")
COST_LOOP(cost_divw, "divw %[t],%[acc],%[k]")
COST_LOOP(cost_subfic, "subfic %[t],%[acc],32")
COST_LOOP(cost_srawi_addze, "srawi %[t],%[acc],1\naddze %[t],%[t]")
COST_LOOP(cost_branch_taken, "cmpwi 7,%[k],3\nbeq 7,2f\nnop\n2:")
COST_LOOP(cost_branch_flip, "andi. %[t],%[acc],1\nbeq 2f\nnop\n2:\nli %[t],1")

/* The vector float instructions the MPEG-2 transform is made of (libavcodec/ppc/mpeg_xenon_vmx.h), eight of each a
 * turn, independent of one another or each waiting for the last; v1 = 1.0, v2 = 0.0 and v0 starts as 5.0. */
#define COST_LOOP_V(fn, body) \
    static __attribute__((noinline)) uint32_t fn(uint32_t n, uint32_t *mem, uint32_t k) \
    { \
        uint32_t acc = 1, t = 0; \
        __asm__ volatile("vspltisw 1,1\nvcfsx 1,1,0\nvspltisw 2,0\nvspltisw 0,5\nvcfsx 0,0,0\nmtctr %[n]\n1:\n" \
                         body "\nadd %[acc],%[acc],%[t]\nbdnz 1b" \
                         : [acc] "+b"(acc), [t] "+b"(t) \
                         : [n] "b"(n), [mem] "b"(mem), [k] "b"(k) \
                         : "ctr", "cr0", "xer", "memory", "lr", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", \
                           "v9", "v10"); \
        return acc; \
    }
#define EIGHT(a, b, c, d, e, f, g, h) a "\n" b "\n" c "\n" d "\n" e "\n" f "\n" g "\n" h
#define SAME8(x) EIGHT(x, x, x, x, x, x, x, x)
COST_LOOP_V(cost_v_none, "nop")
COST_LOOP_V(cost_vmaddfp_apart, EIGHT("vmaddfp 3,0,1,2", "vmaddfp 4,0,1,2", "vmaddfp 5,0,1,2", "vmaddfp 6,0,1,2",
                                      "vmaddfp 7,0,1,2", "vmaddfp 8,0,1,2", "vmaddfp 9,0,1,2", "vmaddfp 10,0,1,2"))
COST_LOOP_V(cost_vmaddfp_chain, SAME8("vmaddfp 0,0,1,2"))
COST_LOOP_V(cost_vnmsubfp_apart, EIGHT("vnmsubfp 3,0,1,2", "vnmsubfp 4,0,1,2", "vnmsubfp 5,0,1,2", "vnmsubfp 6,0,1,2",
                                       "vnmsubfp 7,0,1,2", "vnmsubfp 8,0,1,2", "vnmsubfp 9,0,1,2", "vnmsubfp 10,0,1,2"))
COST_LOOP_V(cost_vcfsx_apart, EIGHT("vcfsx 3,2,0", "vcfsx 4,2,0", "vcfsx 5,2,0", "vcfsx 6,2,0", "vcfsx 7,2,0",
                                    "vcfsx 8,2,0", "vcfsx 9,2,0", "vcfsx 10,2,0"))
COST_LOOP_V(cost_vctsxs_apart, EIGHT("vctsxs 3,0,0", "vctsxs 4,0,0", "vctsxs 5,0,8", "vctsxs 6,0,8", "vctsxs 7,0,0",
                                     "vctsxs 8,0,0", "vctsxs 9,0,8", "vctsxs 10,0,8"))
COST_LOOP_V(cost_convert_chain, "vctsxs 3,0,0\nvcfsx 3,3,0\nvctsxs 3,3,0\nvcfsx 3,3,0\nvctsxs 3,3,0\nvcfsx 3,3,0\n"
                                "vctsxs 3,3,0\nvcfsx 3,3,0")
COST_LOOP_V(cost_vadduwm8, EIGHT("vadduwm 3,2,2", "vadduwm 4,2,2", "vadduwm 5,2,2", "vadduwm 6,2,2", "vadduwm 7,2,2",
                                 "vadduwm 8,2,2", "vadduwm 9,2,2", "vadduwm 10,2,2"))
COST_LOOP_V(cost_vadduwm_chain, SAME8("vadduwm 3,3,2"))
COST_LOOP_V(cost_vsraw8, EIGHT("vsraw 3,2,2", "vsraw 4,2,2", "vsraw 5,2,2", "vsraw 6,2,2", "vsraw 7,2,2",
                               "vsraw 8,2,2", "vsraw 9,2,2", "vsraw 10,2,2"))
COST_LOOP_V(cost_vand8, EIGHT("vand 3,2,2", "vand 4,2,2", "vand 5,2,2", "vand 6,2,2", "vand 7,2,2", "vand 8,2,2",
                              "vand 9,2,2", "vand 10,2,2"))
COST_LOOP_V(cost_vupkhsh8, EIGHT("vupkhsh 3,2", "vupklsh 4,2", "vupkhsh 5,2", "vupklsh 6,2", "vupkhsh 7,2",
                                 "vupklsh 8,2", "vupkhsh 9,2", "vupklsh 10,2"))
COST_LOOP_V(cost_vpk8, EIGHT("vpkuwum 3,2,2", "vpkswss 4,2,2", "vpkuwum 5,2,2", "vpkswss 6,2,2", "vpkshus 7,2,2",
                             "vpkshus 8,2,2", "vpkuwum 9,2,2", "vpkswss 10,2,2"))
COST_LOOP_V(cost_vmrg8, EIGHT("vmrghh 3,2,2", "vmrglh 4,2,2", "vmrghw 5,2,2", "vmrglw 6,2,2", "vmrghh 7,2,2",
                              "vmrglh 8,2,2", "vmrghw 9,2,2", "vmrglw 10,2,2"))
COST_LOOP_V(cost_vperm8, EIGHT("vperm 3,2,2,2", "vperm 4,2,2,2", "vperm 5,2,2,2", "vperm 6,2,2,2", "vperm 7,2,2,2",
                               "vperm 8,2,2,2", "vperm 9,2,2,2", "vperm 10,2,2,2"))
COST_LOOP_V(cost_vsel8, EIGHT("vsel 3,2,2,2", "vcmpequw 4,2,2", "vsel 5,2,2,2", "vcmpequw 6,2,2", "vsel 7,2,2,2",
                              "vcmpequw 8,2,2", "vsel 9,2,2,2", "vcmpequw 10,2,2"))
COST_LOOP_V(cost_vsplt8, EIGHT("vspltisw 3,8", "vspltisw 4,14", "vspltisw 5,8", "vspltisw 6,14", "vspltisw 7,8",
                               "vspltisw 8,14", "vspltisw 9,8", "vspltisw 10,14"))
COST_LOOP_V(cost_mixed, "vcfsx 3,2,0\nvmaddfp 4,3,1,2\nvctsxs 5,4,0\nvadduwm 6,5,2\nvcfsx 7,6,0\nvmaddfp 8,7,1,2\n"
                        "vctsxs 9,8,0\nvadduwm 10,9,2")

void ff_simple_idct_add_int16_8bit(uint8_t *dest, ptrdiff_t line_size, int16_t *block);
void ff_xenon_idct_add(uint8_t *dest, ptrdiff_t stride, int16_t *block);
int ff_xenon_dc_add(uint8_t *dest, ptrdiff_t stride, const int16_t *block);

/* What the MPEG-2 transform's instructions cost, and the transform itself on blocks of four kinds: FFmpeg's in C,
 * the vector one, and the one-coefficient shortcut; everything in the cache. */
static void mpeg2_costs(void)
{
    static uint32_t memory[64] __attribute__((aligned(128)));
    static const struct {
        const char *name;
        uint32_t (*fn)(uint32_t, uint32_t *, uint32_t);
    } loops[] = {
        { "nothing (the loop itself)", cost_v_none },
        { "8 vmaddfp, apart", cost_vmaddfp_apart },
        { "8 vmaddfp, a chain", cost_vmaddfp_chain },
        { "8 vnmsubfp, apart", cost_vnmsubfp_apart },
        { "8 vcfsx, apart", cost_vcfsx_apart },
        { "8 vctsxs, apart", cost_vctsxs_apart },
        { "4 vctsxs + 4 vcfsx, a chain", cost_convert_chain },
        { "8 vadduwm, apart", cost_vadduwm8 },
        { "8 vadduwm, a chain", cost_vadduwm_chain },
        { "8 vsraw", cost_vsraw8 },
        { "8 vand", cost_vand8 },
        { "8 vupkhsh/vupklsh", cost_vupkhsh8 },
        { "8 packs (vpkuwum, vpkswss, vpkshus)", cost_vpk8 },
        { "8 merges", cost_vmrg8 },
        { "8 vperm", cost_vperm8 },
        { "4 vsel + 4 vcmpequw", cost_vsel8 },
        { "8 vspltisw", cost_vsplt8 },
        { "convert, multiply, convert, add: twice, a chain", cost_mixed },
    };
    enum { N = 4000000 };
    xsdk_thread_move(2);
    printf("Vector instructions of the MPEG-2 transform, in a loop of %d (cycles a turn):\n", N);
    double base = 0;
    for (unsigned i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        loops[i].fn(N / 10, memory, 3);
        const uint64_t t0 = now_us();
        const uint32_t result = loops[i].fn(N, memory, 3);
        const double cycles = (double)(now_us() - t0) * 3200.0 / N;
        if (!i)
            base = cycles;
        printf("  %-48s %6.1f (%+6.1f on the empty loop)  [%08x]\n", loops[i].name, cycles, cycles - base,
               (unsigned)result);
    }

    static int16_t blocks[64][64] __attribute__((aligned(128)));
    static uint8_t pixels[8 * 32] __attribute__((aligned(128)));
    static const char *const kinds[] = { "every coefficient set", "rows 0 to 3 set, and the mismatch bit",
                                         "3 coefficients in the top left corner", "DC and the mismatch bit" };
    enum { ROUNDS = 3000 };
    for (int kind = 0; kind < 4; kind++) {
        double cycles[3] = { 0 };
        for (int which = 0; which < 3; which++) {
            if (which == 2 && kind != 3)
                continue;
            uint64_t ticks = 0;
            uint32_t seed = 360;
            for (int round = 0; round < ROUNDS; round++) {
                /* 64 fresh blocks a round, as the C transform works in place; filled outside the timing. */
                memset(blocks, 0, sizeof blocks);
                for (int b = 0; b < 64; b++)
                    for (int i = 0; i < 64; i++) {
                        seed = seed * 1664525u + 1013904223u;
                        const int v = (int)(seed >> 20) % 61 - 30;
                        const bool set = kind == 0 || (kind == 1 && (i < 32 || i == 63)) ||
                                         (kind == 2 && (i == 0 || i == 1 || i == 8)) || (kind == 3 && (i == 0 || i == 63));
                        blocks[b][i] = !set ? 0 : i == 63 && kind ? 1 : v ? v : 1;
                    }
                memset(pixels, 128, sizeof pixels);
                const uint64_t t0 = __builtin_ppc_get_timebase();
                for (int b = 0; b < 64; b++) {
                    if (which == 0)
                        ff_simple_idct_add_int16_8bit(pixels + (b & 1) * 8, 32, blocks[b]);
                    else if (which == 1)
                        ff_xenon_idct_add(pixels + (b & 1) * 8, 32, blocks[b]);
                    else
                        ff_xenon_dc_add(pixels + (b & 1) * 8, 32, blocks[b]);
                }
                ticks += __builtin_ppc_get_timebase() - t0;
            }
            cycles[which] = (double)ticks / ticks_a_ms * 3200000.0 / (ROUNDS * 64.0);
        }
        printf("  a block with %-40s C %5.0f cycles, vector %5.0f", kinds[kind], cycles[0], cycles[1]);
        if (kind == 3)
            printf(", shortcut %5.0f", cycles[2]);
        printf("\n");
    }
    printf("\n");
    fflush(stdout);
}

static void instruction_costs(void)
{
    static uint32_t memory[64] __attribute__((aligned(128)));
    static const struct {
        const char *name;
        uint32_t (*fn)(uint32_t, uint32_t *, uint32_t);
    } loops[] = {
        { "add (the loop itself)", cost_add },
        { "shift by a constant", cost_shift_const },
        { "shift by a register (slw)", cost_shift_var },
        { "arithmetic shift, constant", cost_sraw_const },
        { "arithmetic shift, register", cost_sraw_var },
        { "rotate by a register (rlwnm)", cost_rotate_var },
        { "shift setting flags (rlwinm.)", cost_shift_record },
        { "16-bit load + extsh", cost_load_extend },
        { "16-bit signed load (lha)", cost_load_lha },
        { "store, load elsewhere", cost_store_load_apart },
        { "store, load the same word", cost_store_load_same },
        { "multiply (mullw)", cost_multiply },
        { "vector load (lvx)", cost_lvx },
        { "vector store (stvx)", cost_stvx },
        { "vector store, load the same", cost_stvx_lvx_same },
        { "word store, vector load of it", cost_stw_lvx_same },
        { "lvsl", cost_lvsl },
        { "vperm", cost_vperm },
        { "vector add (vaddubm)", cost_vaddubm },
        { "store a vector's word (stvewx)", cost_stvewx },
        { "dcbt, line in the cache", cost_dcbt },
        { "dcbtst, line in the cache", cost_dcbtst },
        { "word load (lwz)", cost_lwz },
        { "word load, unaligned", cost_lwz_unaligned },
        { "word store (stw)", cost_stw },
        { "byte load (lbz)", cost_lbz },
        { "call and return (bl, blr)", cost_bctrl },
        { "lhz + extsh", cost_lhz_extsh },
        { "andi.", cost_andi_record },
        { "extsb", cost_extsb },
        { "cntlzw", cost_cntlzw },
        { "mfcr", cost_mfcr },
        { "indexed load (lwzx)", cost_lwzx },
        { "divide (divw)", cost_divw },
        { "subfic", cost_subfic },
        { "srawi + addze", cost_srawi_addze },
        { "branch, always taken", cost_branch_taken },
        { "branch, taken every other turn", cost_branch_flip },
    };
    enum { N = 20000000 };
    xsdk_thread_move(2);
    printf("One instruction's cost, in a loop of %d on one hardware thread (cycles at 3.2 GHz):\n", N);
    double base = 0;
    for (unsigned i = 0; i < sizeof loops / sizeof loops[0]; i++) {
        loops[i].fn(N / 10, memory, 3);
        const uint64_t t0 = now_us();
        const uint32_t result = loops[i].fn(N, memory, 3);
        const double cycles = (double)(now_us() - t0) * 3200.0 / N;
        if (!i)
            base = cycles;
        printf("  %-30s %6.1f cycles a turn (%+6.1f on the add loop)  [%08x]\n", loops[i].name, cycles, cycles - base,
               (unsigned)result);
    }
    printf("\n");
    fflush(stdout);
}
#endif

int main(void)
{
    printf("decbench (%s): 1080p, P frames only, 6 slices unless said, 6 threads, 60 frames a clip\n", DECBENCH_FLAVOUR);
    printf("30 fps needs 33.3 ms a frame, 60 fps 16.7 ms\n\n");
    fflush(stdout);
#ifdef DECBENCH_VMX
    if (strstr(devreturn_args(), "tune")) {
        /* The timebase's rate, and what reading it costs (the profile reads it up to six times a macroblock). */
        const uint64_t us0 = now_us(), tb0 = __builtin_ppc_get_timebase();
        while (now_us() - us0 < 200000)
            ;
        ticks_a_ms = (double)(__builtin_ppc_get_timebase() - tb0) / ((double)(now_us() - us0) / 1000.0);
        enum { READS = 2000000 };
        uint64_t acc = 0;
        const uint64_t r0 = now_us();
        for (int i = 0; i < READS; i++)
            acc += __builtin_ppc_get_timebase();
        printf("timebase: %.0f ticks a ms; reading it takes %.0f cycles [%08x]\n\n", ticks_a_ms,
               (double)(now_us() - r0) * 3200.0 / READS, (unsigned)acc);
        instruction_costs();
        if (strstr(devreturn_args(), "memory"))
            memory_tests();
        if (strstr(devreturn_args(), "mpeg2"))
            mpeg2_costs();
        if (strstr(devreturn_args(), "mpeg2"))
            for (unsigned i = 0; i < sizeof mpeg2_passes / sizeof mpeg2_passes[0]; i++)
                run(&mpeg2_passes[i]);
        else
            for (unsigned i = 0; i < sizeof tune_passes / sizeof tune_passes[0]; i++)
                run(&tune_passes[i]);
        printf("\ndecbench tune done\n");
        fflush(stdout);
        devreturn();
        return 0;
    }
    instruction_costs();
#endif
    for (unsigned i = 0; i < sizeof passes / sizeof passes[0]; i++)
        run(&passes[i]);
    xsdk_thread_move(0);
    if (NT_SUCCESS(xsdk_gpu_open())) {
        upload_test(true);
        upload_test(false);
        xsdk_gpu_close();
    }
    printf("\ndecbench done\n");
    fflush(stdout);
#ifdef DECBENCH_NEXT
    devchain(DECBENCH_NEXT);   /* the AltiVec build next, then back to devlaunch */
#endif
    devreturn();
    return 0;
}
