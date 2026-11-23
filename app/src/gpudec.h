/* Experimental GPU decoding (the hybrid decoder): FFmpeg parses on the CPU, the GPU predicts each frame and adds its
 * residual (src/gpumc.c), the CPU rebuilds the intra macroblocks. Off unless the setting asks for it; any failure
 * turns it off for the rest of the stream and decoding goes on on the CPU. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct AVFrame;

/* Before a stream starts (the setting). */
void gpudec_set_wanted(bool on);
bool gpudec_wanted(void);

/* Decode thread. At stream set-up: has the drawing thread set the GPU up for coded_width x coded_height and points
 * FFmpeg's hook at it; false (and the CPU decodes) if it can't. */
bool gpudec_start(int coded_width, int coded_height);
bool gpudec_active(void);
/* Before each avcodec_send_packet. */
void gpudec_begin_frame(void);
/* After a frame comes out of FFmpeg: the GPU's part (through the drawing thread), then the CPU's replay. False if it
 * failed: the frame is broken, the host should send a keyframe, and the CPU decodes from now on. */
bool gpudec_finish_frame(struct AVFrame *frame);
/* At stream end. */
void gpudec_stop(void);
/* The last second's average GPU and replay times, for the stats line. */
void gpudec_times(float *gpu_ms, float *replay_ms);

/* Drawing thread: once a frame, after xsdk_present_begin and before xsdk_gpu_set_target. Runs what the decode thread
 * asked for. */
void gpudec_service(void);
