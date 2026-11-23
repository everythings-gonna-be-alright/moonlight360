/* The GPU half of the hybrid H.264 decoder: inter prediction of a whole picture from FFmpeg's motion vector grid
 * (h264_xenon.h), drawn four pixels a texel, resolved, and copied into the picture's planes. Drawing thread only. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* In: split waits after the luma pass to time it alone; sdk_download un-tiles with the SDK's download instead. */
    bool split, sdk_download;
    /* Out. */
    uint32_t flush_us;    /* writing the grid out of the CPU's cache */
    uint32_t gpu_us;      /* the three passes and their resolves, to the fence */
    uint32_t luma_us;     /* of which the luma passes (exact when split) */
    uint32_t pass_us[5];  /* when split: the luma passes G, B, H, J and the final one */
    uint32_t detile_us;   /* the resolved texels into the picture's planes */
} GpuMcTimes;

/* For pictures of coded_width x coded_height (multiples of 16). */
bool gpumc_init(int coded_width, int coded_height);
void gpumc_shutdown(void);
/* The grid FFmpeg writes (xenon_hybrid_set_grid): a float per 4x4 luma block. */
uint32_t *gpumc_grid(int *pitch_blocks, int *rows_blocks);
/* The residual buffers FFmpeg writes (xenon_hybrid_set_residual): luma, Cb, Cr as int16, pitches in samples (luma,
 * chroma), and the flags, a byte a macroblock. */
void gpumc_residual(int16_t *planes[3], int pitch[2], uint8_t **flags, int *flags_pitch);
/* Their tiled layout, for xenon_hybrid_set_residual_layout; and one sample read back through it (for checks). */
void gpumc_residual_layout(const uint32_t *offsets[3], uint32_t texels[3], uint32_t band[3]);
int gpumc_residual_sample(int plane, int x, int y);
/* Finds which memory byte each output channel lands in after a resolve, and checks the grid reads back as written.
 * Writes what it found to report; false if the GPU can't be used. Before the first gpumc_predict. */
bool gpumc_calibrate(char *report, size_t size);
/* Steps toward gpumc_predict, for hybench: a plain copy of the luma plane (no vectors), chroma alone, luma alone,
 * each into dst's plane. False if the GPU didn't finish; nothing is sent to it after that. */
enum { GPUMC_TEST_COPY, GPUMC_TEST_CHROMA, GPUMC_TEST_LUMA };
/* Smaller steps still, to find what hangs the GPU, each the copy shader into a 32x8 target unless said: reading the
 * grid texture (R32F, known good); a 64x64 R8 texture of the SDK's own; a full-size one, linear filtering; the same,
 * point sampling; the frame where FFmpeg decoded it; and that into the full 480x1088 target. 1 if it finished, 0 if
 * the GPU hung (nothing is sent to it after that), -1 if it couldn't be set up. */
enum {
    GPUMC_TRY_GRID_AS_REFERENCE,
    GPUMC_TRY_SMALL_R8,
    GPUMC_TRY_BIG_R8_LINEAR,
    GPUMC_TRY_BIG_R8_POINT,
    GPUMC_TRY_FRAME_POINT,
    GPUMC_TRY_FRAME_BIG_TARGET,
    GPUMC_TRIES,
};
int gpumc_try(int what, uint8_t *const ref[3], const int pitch[3]);
bool gpumc_test(int stage, uint8_t *const ref[3], uint8_t *const dst[3], const int pitch[3]);
/* Predicts the picture into dst from ref, both in memory the GPU reads (linear R8 planes, 4 KB aligned), dst's rows
 * pitch bytes apart as ref's. The grid must hold this picture's vectors. */
bool gpumc_predict(uint8_t *const ref[3], uint8_t *const dst[3], const int pitch[3], GpuMcTimes *times);
