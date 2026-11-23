#include "video.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>

#include "gpudec.h"
#include "placement.h"
#include "xsdk/kernel.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

enum {
    /* Decoding threads: the decode thread plus 5 FFmpeg slice workers, one on each hardware thread. */
    DECODE_THREADS = 6,
    /* Slices per frame asked of the host, one per decoding thread. */
    SLICES_PER_FRAME = 6,
    /* The decode thread's hardware thread. */
    DECODE_HW_THREAD = 2,
    /* Decode threads' priority. -2 was tried on a misread probe result and slowed decoding to ~30 ms a frame; the
     * network deaths it was meant to stop happen with the CPU idle too (2026-11-22). The kernel keeps -3..4. */
    DEFAULT_DECODE_PRIORITY = 0,
};
/* Workers on all three cores: on two (hw 2-5 only) 1080p CABAC took 91 ms a frame, on three 64 ms (decbench,
 * 2026-11-23). On core 0 they sit under the threads that must not wait: Moonlight's network threads (+2), the audio
 * feeder (+1) and the main thread (+1, main.c), so they only take what those leave. */
static const uint32_t worker_hw_threads[] = { 3, 4, 5, 0, 1 };
static bool fast_decode;
static bool decode_enabled = true;
static int decode_priority = DEFAULT_DECODE_PRIORITY;

void video_set_decode_priority(int priority)
{
    decode_priority = priority;
}

void video_set_decode_enabled(bool enabled)
{
    decode_enabled = enabled;
}

void video_set_fast_decode(bool fast)
{
    fast_decode = fast;
}

/* Reads entropy_coding_mode_flag from an H.264 picture parameter set (NAL type 8): after the NAL header come
 * pic_parameter_set_id and seq_parameter_set_id (both ue(v)), then the flag. Returns 1 CABAC, 0 CAVLC, -1 unknown. */
static int pps_cabac(const uint8_t *nal, int size)
{
    uint8_t rbsp[16];
    int n = 0, zeros = 0;
    for (int i = 1; i < size && n < (int)sizeof rbsp; i++) {   /* skip emulation-prevention bytes */
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        rbsp[n++] = nal[i];
    }
    int bit = 0;
#define READ_BIT() (bit < n * 8 ? (rbsp[bit / 8] >> (7 - bit % 8)) & 1 : -1); bit++
    for (int field = 0; field < 2; field++) {   /* two ue(v) values */
        int leading = 0;
        for (;;) {
            int b = READ_BIT();
            if (b < 0 || leading > 31)
                return -1;
            if (b)
                break;
            leading++;
        }
        bit += leading;
    }
    int flag = READ_BIT();
#undef READ_BIT
    return flag;
}

/* Looks for a PPS in a decode unit's Annex B data. */
static int find_cabac(const uint8_t *data, int size)
{
    for (int i = 0; i + 3 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1F) == 8)
            return pps_cabac(data + i + 3, size - i - 3);
    }
    return -1;
}

static const AVCodec *codec;
static AVCodecContext *ctx;
static AVPacket *packet;
static AVFrame *frame;
static uint8_t *packet_buffer;
static int packet_capacity;
static int moved_decode_thread;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static AVFrame *pending;   /* newest decoded frame, handed to the renderer */
static VideoStats stats;
static uint64_t window_start_us, window_decode_us;
static uint32_t window_frames;

static uint64_t now_us(void);

/* Arrivals, for the network graph: a ring of the last NET_HISTORY frames, under lock. Frame numbers count up by one
 * from the host; a gap is frames moonlight-common-c dropped because packets went missing. */
static struct {
    float interval_ms[NET_HISTORY];
    bool lost_before[NET_HISTORY];
    int next, count;
    int last_number;
    uint64_t last_us;
    uint32_t lost_total, window_lost, window_arrived;
    uint64_t window_start_us;
    float loss_pct, budget_ms;
} net;

/* Decode thread, for every frame that arrives (decoded or not). */
static void record_arrival(const DECODE_UNIT *unit)
{
    const uint64_t now = now_us();
    pthread_mutex_lock(&lock);
    const int gap = net.last_number ? unit->frameNumber - net.last_number - 1 : 0;
    const uint32_t lost = gap > 0 ? (uint32_t)gap : 0;
    if (net.last_us) {
        net.interval_ms[net.next] = (float)(now - net.last_us) / 1000.0f;
        net.lost_before[net.next] = lost > 0;
        net.next = (net.next + 1) % NET_HISTORY;
        if (net.count < NET_HISTORY)
            net.count++;
    }
    net.last_us = now;
    net.last_number = unit->frameNumber;
    net.lost_total += lost;
    net.window_lost += lost;
    net.window_arrived++;
    if (now - net.window_start_us >= 1000000) {
        const uint32_t all = net.window_lost + net.window_arrived;
        net.loss_pct = all ? 100.0f * (float)net.window_lost / (float)all : 0.0f;
        net.window_start_us = now;
        net.window_lost = net.window_arrived = 0;
    }
    pthread_mutex_unlock(&lock);
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* ---- frames in memory the GPU reads ----
 * FFmpeg decodes into cached physical memory, laid out as the GPU's linear R8 textures, so render.c draws a frame
 * where it lies: no 6.3 ms copy a 1080p frame (decbench, 2026-11-23). The CPU writes through its cache, so each
 * finished frame is written out to memory (dcbst) before the GPU reads it. */

/* Marks frames from these pools (AVFrame.opaque; FFmpeg keeps it, as AV_CODEC_FLAG_COPY_OPAQUE is off). */
static const char gpu_frame_tag[] = "gpu";
static AVBufferPool *plane_pools[3];
static int pool_width, pool_height;
static int plane_pitch[3];

static void physical_free(void *opaque, uint8_t *data)
{
    (void)opaque;
    MmFreePhysicalMemory(0, data);
}

static AVBufferRef *physical_alloc(void *opaque, size_t size)
{
    (void)opaque;
    const uint32_t page = 64 * 1024;
    const uint32_t rounded = ((uint32_t)size + page - 1) & ~(page - 1);
    uint8_t *memory = MmAllocatePhysicalMemoryEx(0, rounded, PAGE_READWRITE | MEM_LARGE_PAGES, 0, UINT32_MAX, 4096);
    if (!memory) {
        printf("[E] video: no physical memory for a %u-byte plane\n", (unsigned)rounded);
        return NULL;
    }
    AVBufferRef *buffer = av_buffer_create(memory, size, physical_free, NULL, 0);
    if (!buffer)
        MmFreePhysicalMemory(0, memory);
    return buffer;
}

static void release_pools(void)
{
    for (int p = 0; p < 3; p++)
        av_buffer_pool_uninit(&plane_pools[p]);   /* buffers still out free themselves when returned */
    pool_width = pool_height = 0;
}

/* Planes as linear R8 textures of the frame's size lay them out: render.c makes the same textures over them. */
static bool make_pools(int width, int height, int coded_width, int coded_height)
{
    release_pools();
    for (int p = 0; p < 3; p++) {
        const int w = p ? (width + 1) / 2 : width, h = p ? (coded_height + 1) / 2 : coded_height;
        const int cw = p ? (coded_width + 1) / 2 : coded_width;
        XsdkTexture shown, coded;
        if (!NT_SUCCESS(xsdk_texture_init(&shown, XSDK_FORMAT_R8, (uint32_t)w, 1, 1, false)) ||
            !NT_SUCCESS(xsdk_texture_init(&coded, XSDK_FORMAT_R8, (uint32_t)cw, (uint32_t)h, 1, false)) ||
            shown.pitch != coded.pitch || coded.pitch % 16) {
            printf("[W] video: plane %d pitch %u/%u doesn't fit FFmpeg; frames get copied\n", p,
                   (unsigned)shown.pitch, (unsigned)coded.pitch);
            release_pools();
            return false;
        }
        plane_pitch[p] = (int)coded.pitch;
        plane_pools[p] = av_buffer_pool_init2((size_t)coded.pitch * (size_t)h, NULL, physical_alloc, NULL);
        if (!plane_pools[p]) {
            release_pools();
            return false;
        }
    }
    pool_width = width;
    pool_height = height;
    printf("[I] video: frames decoded straight into GPU-readable memory (%dx%d, pitches %d/%d)\n", width, height,
           plane_pitch[0], plane_pitch[1]);
    return true;
}

static int get_buffer(AVCodecContext *c, AVFrame *f, int flags)
{
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)
        return avcodec_default_get_buffer2(c, f, flags);
    int coded_width = f->width, coded_height = f->height;
    int linesize_align[AV_NUM_DATA_POINTERS];
    avcodec_align_dimensions2(c, &coded_width, &coded_height, linesize_align);
    if ((f->width != pool_width || f->height != pool_height || !plane_pools[0]) &&
        !make_pools(f->width, f->height, coded_width, coded_height))
        return avcodec_default_get_buffer2(c, f, flags);
    for (int p = 0; p < 3; p++) {
        f->buf[p] = av_buffer_pool_get(plane_pools[p]);
        if (!f->buf[p]) {
            for (int q = 0; q < p; q++)
                av_buffer_unref(&f->buf[q]);
            return AVERROR(ENOMEM);
        }
        f->data[p] = f->buf[p]->data;
        f->linesize[p] = plane_pitch[p];
    }
    f->extended_data = f->data;
    f->opaque = (void *)gpu_frame_tag;
    return 0;
}

bool video_frame_in_gpu_memory(const AVFrame *f)
{
    return f->opaque == gpu_frame_tag;
}

/* Writes a decoded frame out of the CPU's cache (128-byte lines) so the GPU reads what the decoder wrote. */
static void write_out(const AVFrame *f)
{
    if (!video_frame_in_gpu_memory(f))
        return;
    for (int p = 0; p < 3; p++) {
        const int rows = p ? (f->height + 1) / 2 : f->height;
        const uintptr_t start = (uintptr_t)f->data[p] & ~(uintptr_t)127;
        const uintptr_t end = (uintptr_t)f->data[p] + (uintptr_t)f->linesize[p] * (uintptr_t)rows;
        for (uintptr_t a = start; a < end; a += 128)
            __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

static int setup(int video_format, int width, int height, int redraw_rate, void *context, int dr_flags)
{
    if (!(video_format & VIDEO_FORMAT_MASK_H264)) {
        printf("[E] video: unsupported format 0x%x\n", video_format);
        return -1;
    }
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (!ctx || !packet || !frame)
        return -1;
    ctx->width = width;
    ctx->height = height;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = DECODE_THREADS;
    ctx->thread_type = FF_THREAD_SLICE;
    ctx->get_buffer2 = get_buffer;
    /* Experimental GPU decoding (gpudec.c) needs the deblocking filter off, as fast decode has it. */
    const bool gpu = gpudec_start((width + 15) & ~15, (height + 15) & ~15);
    if (fast_decode || gpu)
        ctx->skip_loop_filter = AVDISCARD_ALL;

    /* FFmpeg creates its slice workers inside avcodec_open2: place them on the other cores. */
    placement_set_worker_priority(decode_priority);
    placement_set_workers(worker_hw_threads, sizeof worker_hw_threads / sizeof worker_hw_threads[0]);
    int ret = avcodec_open2(ctx, codec, NULL);
    placement_set_workers(NULL, 0);
    placement_set_worker_priority(0);
    if (ret < 0) {
        printf("[E] video: avcodec_open2 failed: %d\n", ret);
        return -1;
    }
    memset(&stats, 0, sizeof stats);
    stats.cabac = -1;
    stats.fast_decode = fast_decode || gpu;
    stats.gpu_decode = gpu;
    window_start_us = now_us();
    window_decode_us = 0;
    window_frames = 0;
    pthread_mutex_lock(&lock);
    memset(&net, 0, sizeof net);
    net.window_start_us = window_start_us;
    net.budget_ms = redraw_rate > 0 ? 1000.0f / (float)redraw_rate : 0.0f;
    pthread_mutex_unlock(&lock);
    moved_decode_thread = 0;
    printf("[I] video: H.264 %dx%d@%d, %d decode threads at priority %d, %d slices asked, deblocking %s%s\n",
           width, height, redraw_rate, DECODE_THREADS, decode_priority, SLICES_PER_FRAME, fast_decode ? "skipped (fast decode)" : "on",
           decode_enabled ? "" : "; DECODING OFF (network test)");
    return 0;
}

static void cleanup(void)
{
    pthread_mutex_lock(&lock);
    av_frame_free(&pending);
    pthread_mutex_unlock(&lock);
    gpudec_stop();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    release_pools();
    av_free(packet_buffer);
    packet_buffer = NULL;
    packet_capacity = 0;
}

static int submit(PDECODE_UNIT unit)
{
    if (!moved_decode_thread) {
        placement_move_self(DECODE_HW_THREAD);
        /* The decode thread was created as a network thread (+2): it must not outrank the system either. */
        NTSTATUS st = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, decode_priority, NULL);
        printf("[I] video: decode thread on hw %d at priority %d (0x%08X)\n", DECODE_HW_THREAD, decode_priority,
               (unsigned)st);
        moved_decode_thread = 1;
    }
    record_arrival(unit);
    if (!decode_enabled) {
        /* Network test: count what arrives, decode nothing. */
        pthread_mutex_lock(&lock);
        stats.decoded++;
        pthread_mutex_unlock(&lock);
        return DR_OK;
    }
    int needed = unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE;
    if (needed > packet_capacity) {
        av_free(packet_buffer);
        packet_capacity = needed + needed / 2;
        packet_buffer = av_malloc(packet_capacity);
        if (!packet_buffer) {
            packet_capacity = 0;
            return DR_NEED_IDR;
        }
    }
    int length = 0;
    for (PLENTRY entry = unit->bufferList; entry; entry = entry->next) {
        memcpy(packet_buffer + length, entry->data, entry->length);
        length += entry->length;
    }
    memset(packet_buffer + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    if (stats.cabac < 0 && unit->frameType == FRAME_TYPE_IDR) {
        int cabac = find_cabac(packet_buffer, length);
        if (cabac >= 0) {
            pthread_mutex_lock(&lock);
            stats.cabac = cabac;
            pthread_mutex_unlock(&lock);
            printf("[I] video: host stream uses %s\n", cabac ? "CABAC" : "CAVLC");
        }
    }
    packet->data = packet_buffer;
    packet->size = length;

    uint64_t start = now_us();
    gpudec_begin_frame();
    int ret = avcodec_send_packet(ctx, packet);
    if (ret < 0) {
        stats.errors++;
        return DR_NEED_IDR;
    }
    while ((ret = avcodec_receive_frame(ctx, frame)) == 0) {
        /* With GPU decoding, the frame is only parsed so far: the GPU and the replay finish it. */
        if (gpudec_active() && !gpudec_finish_frame(frame)) {
            av_frame_unref(frame);
            stats.errors++;
            return DR_NEED_IDR;   /* that frame is broken; a keyframe brings the CPU's decoding back in step */
        }
        write_out(frame);
        AVFrame *out = av_frame_clone(frame);
        av_frame_unref(frame);
        if (!out)
            continue;
        pthread_mutex_lock(&lock);
        if (pending) {
            av_frame_free(&pending);
            stats.dropped++;
        }
        pending = out;
        stats.decoded++;
        pthread_mutex_unlock(&lock);
        window_frames++;
    }
    window_decode_us += now_us() - start;

    uint64_t now = now_us();
    if (now - window_start_us >= 1000000) {
        pthread_mutex_lock(&lock);
        stats.decode_ms_avg = window_frames ? window_decode_us / 1000.0f / window_frames : 0;
        stats.gpu_decode = gpudec_active();
        gpudec_times(&stats.gpu_ms, &stats.replay_ms);
        pthread_mutex_unlock(&lock);
        window_start_us = now;
        window_decode_us = 0;
        window_frames = 0;
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        stats.errors++;
        return DR_NEED_IDR;
    }
    return DR_OK;
}

struct AVFrame *video_take_frame(void)
{
    pthread_mutex_lock(&lock);
    AVFrame *out = pending;
    pending = NULL;
    pthread_mutex_unlock(&lock);
    return out;
}

void video_get_stats(VideoStats *out)
{
    pthread_mutex_lock(&lock);
    *out = stats;
    pthread_mutex_unlock(&lock);
}

void video_get_net_stats(NetStats *out)
{
    pthread_mutex_lock(&lock);
    out->lost_frames = net.lost_total;
    out->loss_pct = net.loss_pct;
    out->budget_ms = net.budget_ms;
    out->count = net.count;
    const int first = (net.next - net.count + NET_HISTORY) % NET_HISTORY;
    for (int i = 0; i < net.count; i++) {
        out->interval_ms[i] = net.interval_ms[(first + i) % NET_HISTORY];
        out->lost_before[i] = net.lost_before[(first + i) % NET_HISTORY];
    }
    pthread_mutex_unlock(&lock);
}

DECODER_RENDERER_CALLBACKS video_callbacks = {
    .setup = setup,
    .cleanup = cleanup,
    .submitDecodeUnit = submit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME),
};
