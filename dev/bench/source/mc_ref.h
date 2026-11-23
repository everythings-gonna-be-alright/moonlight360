/* H.264 inter prediction for the hybrid decoder, in plain C (mc_ref.c). */
#pragma once
#include <stdint.h>

int mc_ref_luma(const uint8_t *ref, int stride, int w, int h, int x, int y, int fx, int fy);
int mc_ref_chroma(const uint8_t *ref, int stride, int w, int h, int x, int y, int dx, int dy);
/* Predicts a whole picture, width x height luma (the coded size), from the grid of h264_xenon.h. */
void mc_ref_predict(const uint8_t *const ref[3], const int ref_stride[3], uint8_t *const dst[3], const int dst_stride[3],
                    int width, int height, const uint32_t *grid, int grid_pitch);
/* A residual sample of plane p at (x, y), wherever the caller keeps it. */
typedef int (*McResidualSample)(int plane, int x, int y, void *context);
/* Adds each flagged macroblock's residual (h264_xenon.h, xenon_hybrid_set_residual) to dst and clips. */
void mc_ref_add_residual(uint8_t *const dst[3], const int dst_stride[3], McResidualSample sample, void *context,
                         const uint8_t *flags, int flags_pitch, int width, int height);
