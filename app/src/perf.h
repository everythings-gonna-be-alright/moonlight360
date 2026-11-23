/* Performance readings for the stats overlay: how busy each hardware thread and the GPU are, and the console's
 * temperatures. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* 0-100 for each hardware thread, and their average; -1 until measured. */
    int cpu[6];
    int cpu_average;
    /* Share of the last second the GPU spent on the app's frames, 0-100; -1 until measured. */
    int gpu;
    /* Degrees C, from the system controller; 0 if unknown. */
    float temp_cpu, temp_gpu, temp_edram, temp_board;
    /* Counts the samples (twice a second), so a reader sees when a new one came. */
    uint32_t sample;
} PerfReadings;

/* Starts the idle probes and the sampler (once). */
void perf_start(void);
void perf_get(PerfReadings *out);

/* The GPU's share: call perf_gpu_frame_begin after xsdk_gpu_set_target and perf_gpu_frame_end after the resolve,
 * on the drawing thread. */
void perf_gpu_frame_begin(void);
void perf_gpu_frame_end(void);
