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
    mpeg2_13_end[], mpeg2_26_start[], mpeg2_26_end[];
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
    ff_h264_mb_hook = pass->parse_only ? parse_only_hook : NULL;
    ff_h264_skip_mc = pass->no_mc;
    ff_xenon_chroma_c = pass->chroma_c;
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
    fflush(stdout);
    (void)busy_us;
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
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
                         : "ctr", "cr0", "xer", "memory"); \
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
