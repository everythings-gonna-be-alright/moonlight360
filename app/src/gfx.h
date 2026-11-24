/* 2D drawing for the menus: rounded rectangles, soft shadows, circles, text helpers and controller button icons.
 * Drawing thread only, between gfx_frame_begin (after xsdk_gpu_set_target) and the resolve. Colours are 0xAARRGGBB. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool gfx_init(void);
void gfx_shutdown(void);
/* Starts a frame on a target of width x height pixels. */
void gfx_frame_begin(uint32_t width, uint32_t height);
/* Draws the shapes queued so far. Text drawn through gfx does it itself; call it before the resolve and before
 * drawing anything not through gfx over shapes. */
void gfx_flush(void);
/* After the frame's resolve: the GPU work that reads this frame's shapes. */
void gfx_frame_end(void);
/* Icon index (icons.h's Icon) from the font's atlas, smooth, in its FONT_ICON_PX square with its top left at (x, y). */
void gfx_icon(int index, float x, float y, uint32_t argb);
/* Until gfx_scale_end, everything is drawn scale times its size about (origin_x, origin_y): shapes, images' places and
 * text, scale-1 text in a font of its own at 80% so it stays sharp (the only scale it has, 0.8). Positions and
 * gfx_text_width stay in the caller's unscaled units. */
void gfx_scale_begin(float scale, float origin_x, float origin_y);
/* The same, and moved: a point p is drawn at to + (p - from) * scale. gfx_scale_end ends either. */
void gfx_transform_begin(float scale, float from_x, float from_y, float to_x, float to_y);
void gfx_scale_end(void);
/* An image in GPU memory (RGBA bytes, row after row), drawn with gfx_image. */
typedef struct GfxImage GfxImage;
GfxImage *gfx_image_create(const uint8_t *rgba, int width, int height);
/* Waits for the GPU to be done with it first. */
void gfx_image_free(GfxImage *image);
/* The image stretched over (x, y, w, h), its colours times argb's (0xFFFFFFFF as it is; lower alpha fades it). */
void gfx_image(const GfxImage *image, float x, float y, float w, float h, uint32_t argb);
/* Frames since start, for animation. */
uint32_t gfx_frame(void);

/* A rectangle with corners rounded by radius (0 for square). */
void gfx_rect(float x, float y, float w, float h, float radius, uint32_t argb);
/* The same, shaded from top_argb at the top to bottom_argb at the bottom. */
void gfx_rect_v(float x, float y, float w, float h, float radius, uint32_t top_argb, uint32_t bottom_argb);
/* A soft shadow or glow under a rectangle: argb fading out over blur pixels around its edge. */
void gfx_shadow(float x, float y, float w, float h, float radius, float blur, uint32_t argb);
void gfx_circle(float cx, float cy, float r, uint32_t argb);

/* Text in the 10x20 font, each cell scale times that. */
void gfx_text(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
int32_t gfx_text_width(const char *utf8, uint32_t scale);
void gfx_text_center(int32_t center_x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
void gfx_text_right(int32_t right, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
/* Copies in, cut to max_chars characters with an ellipsis if longer. */
void gfx_fit(char *out, size_t size, const char *in, int max_chars);
/* Copies in, with line breaks at spaces so no line passes max_chars characters. Returns the line count. */
int gfx_wrap(char *out, size_t size, const char *in, int max_chars);

/* Controller buttons: 'A', 'B', 'X', 'Y', 'L' (LB), 'R' (RB), 'K' (Back), 'S' (Start), 'D' (D-pad). Drawn with
 * their left edge at x, centred on center_y; returns the width taken. */
int32_t gfx_button(int32_t x, int32_t center_y, char button);
/* A button and its label; returns the width taken, gap included. */
int32_t gfx_hint(int32_t x, int32_t center_y, char button, const char *label, uint32_t argb);
/* Eight dots turning around (cx, cy). */
void gfx_spinner(float cx, float cy, float r, uint32_t argb);
/* The app's logo, a crescent moon on a tile of size pixels. */
void gfx_logo(float x, float y, float size);
