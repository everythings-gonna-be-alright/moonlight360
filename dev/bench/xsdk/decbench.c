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

/* The clips: one scene, 60 frames of 1080p30 with 6 slices, at three bitrates; each one's checksum decoded by FFmpeg
 * on the Mac (dev/bench/data, from dev/make_clips.sh; deblocking on, then skipped). 1080p60 at 25 Mbit/s carries as many bits a frame as 1080p30
 * at 12.5, so the middle clips are a 1080p60 stream's frames. */
typedef struct {
    const char *name;
    const uint8_t *start, *end;
    uint32_t checksum, checksum_skip;
} Clip;
static const Clip clips[] = {
    { "CABAC 22.5 Mbps", cabac22_start, cabac22_end, 0xaf395430, 0xecfa8111 },
    { "CABAC 11.6 Mbps", cabac12_start, cabac12_end, 0x5e91c24b, 0xf8b09897 },
    { "CAVLC 22.6 Mbps", cavlc22_start, cavlc22_end, 0x3aae36b1, 0x736ed62b },
    { "CAVLC 11.5 Mbps", cavlc12_start, cavlc12_end, 0x4eebed01, 0xf22cd854 },
    { "CAVLC  5.4 Mbps", cavlc5_start, cavlc5_end, 0x6c9dec95, 0xc5d3c8e0 },
};

typedef struct {
    const char *name;
    int clip;
    int threads;
    bool skip_deblock;
    bool parse_only;   /* every macroblock parsed, none reconstructed: what stays on the CPU in a GPU hybrid */
    bool no_mc;        /* everything but motion compensation: the difference is what a GPU MC pass takes over */
    uint32_t main_hw;
    uint32_t workers[6];
    int worker_count;
} Pass;

/* All passes as the app decodes now: 6 slice threads on the three cores. */
#define ALL_HW 6, false, false, false, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_SKIP 6, true, false, false, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_NO_MC 6, true, false, true, 2, { 3, 4, 5, 0, 1 }, 5
#define ALL_HW_PARSE 6, true, true, false, 2, { 3, 4, 5, 0, 1 }, 5
static const Pass passes[] = {
    { "deblock", 0, ALL_HW }, { "skip deblock", 0, ALL_HW_SKIP }, { "no MC", 0, ALL_HW_NO_MC }, { "PARSE ONLY", 0, ALL_HW_PARSE },
    { "deblock", 1, ALL_HW }, { "skip deblock", 1, ALL_HW_SKIP }, { "no MC", 1, ALL_HW_NO_MC }, { "PARSE ONLY", 1, ALL_HW_PARSE },
    { "deblock", 2, ALL_HW }, { "skip deblock", 2, ALL_HW_SKIP }, { "no MC", 2, ALL_HW_NO_MC }, { "PARSE ONLY", 2, ALL_HW_PARSE },
    { "deblock", 3, ALL_HW }, { "skip deblock", 3, ALL_HW_SKIP }, { "no MC", 3, ALL_HW_NO_MC }, { "PARSE ONLY", 3, ALL_HW_PARSE },
    { "deblock", 4, ALL_HW }, { "skip deblock", 4, ALL_HW_SKIP }, { "no MC", 4, ALL_HW_NO_MC }, { "PARSE ONLY", 4, ALL_HW_PARSE },
};

extern void (*ff_h264_mb_hook)(const void *h, void *sl);
extern int ff_h264_skip_mc;
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

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->thread_count = pass->threads;
    if (pass->skip_deblock)
        ctx->skip_loop_filter = AVDISCARD_ALL;
    ff_h264_mb_hook = pass->parse_only ? parse_only_hook : NULL;
    ff_h264_skip_mc = pass->no_mc;
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
            const uint8_t *au = next_access_unit(p, end, &au_end);
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
    const uint32_t expected = pass->skip_deblock ? clip->checksum_skip : clip->checksum;
    printf("%-15s %-13s avg %6.2f ms (%5.1f fps)  p95 %6.2f ms  %08x %s\n", clip->name, pass->name, avg,
           avg > 0 ? 1000.0 / avg : 0, gap_count ? gaps[gap_count * 95 / 100] / 1000.0 : 0, (unsigned)sum,
           pass->parse_only ? "(no picture: parsing alone)"
           : pass->no_mc   ? "(no picture: no motion compensation)"
           : sum == expected ? "matches the Mac"
                             : "MISMATCH");
    ff_h264_mb_hook = NULL;
    ff_h264_skip_mc = 0;
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

int main(void)
{
    printf("decbench (%s): 1080p, High profile, P only, 6 slices, 6 threads, 60 frames a clip\n", DECBENCH_FLAVOUR);
    printf("30 fps needs 33.3 ms a frame, 60 fps 16.7 ms\n\n");
    fflush(stdout);
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
