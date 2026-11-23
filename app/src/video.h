/* H.264 decoding with FFmpeg, fed by moonlight-common-c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <Limelight.h>

struct AVFrame;

extern DECODER_RENDERER_CALLBACKS video_callbacks;

/* Before a stream starts: skip the deblocking filter or not. */
void video_set_fast_decode(bool fast);
/* Before a stream starts: false receives video but never decodes it (a network test). */
void video_set_decode_enabled(bool enabled);
/* Before a stream starts: the decode threads' priority (default 0). */
void video_set_decode_priority(int priority);

/* The newest decoded frame not yet taken, or NULL; the caller frees it with av_frame_free. */
struct AVFrame *video_take_frame(void);

typedef struct {
    uint32_t decoded;      /* frames decoded since the stream started */
    uint32_t dropped;      /* decoded frames replaced before the renderer took them */
    uint32_t errors;       /* decode errors (each asks the host for an IDR frame) */
    float decode_ms_avg;   /* average decode time over the last second */
    int cabac;             /* entropy coding the host's stream uses: 1 CABAC, 0 CAVLC, -1 not seen yet */
    bool fast_decode;
    bool gpu_decode;       /* experimental GPU decoding is running */
    float gpu_ms, replay_ms;   /* with it: the GPU's part and the CPU's replay, last second's average a frame */
} VideoStats;

void video_get_stats(VideoStats *stats);

/* Whether FFmpeg decoded the frame into GPU-readable memory as linear R8 planes (render.c then draws it in place). */
bool video_frame_in_gpu_memory(const struct AVFrame *frame);
