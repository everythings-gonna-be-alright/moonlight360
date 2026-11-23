/* Draws decoded YUV 4:2:0 frames full-screen with the GPU. Drawing thread only. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct AVFrame;

bool render_init(void);
/* Before the first frame: Xenos-tiled textures (the 360's normal layout) or linear ones. */
void render_set_tiled(bool tiled);
void render_shutdown(void);
/* Uploads a new frame (and keeps showing it until the next). Takes ownership of the frame. */
void render_set_frame(struct AVFrame *frame);
/* Draws the current frame, if any, into the target set for this frame. Returns whether it drew. */
bool render_draw(uint32_t target_width, uint32_t target_height);
/* After xsdk_present_end: remembers which GPU work last read the textures. */
void render_frame_submitted(void);
/* Drops the current frame (at the end of a stream). */
void render_clear(void);
