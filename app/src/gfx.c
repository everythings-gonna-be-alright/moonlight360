#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef GPUMC_SHADERS
#include GPUMC_SHADERS   /* a bench's own shader header */
#else
#include "moonlight360_shaders.h"
#endif
#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/text.h"

enum {
    BLOCKS = 3,               /* frames of shapes in flight: one written while the GPU may still read the others */
    SHAPES_PER_BLOCK = 1024,
    CORNERS = 4,
    VERTEX_FLOATS = 12,       /* position 2, colour 4, local 2, shape 4 */
    STRIDE_BYTES = VERTEX_FLOATS * 4,
    SHAPE_BYTES = CORNERS * STRIDE_BYTES,   /* 192: a multiple of 64, as text.c's glyphs are */
    BLOCK_BYTES = SHAPES_PER_BLOCK * SHAPE_BYTES,
    FENCE_TIMEOUT_MS = 500,
};

/* Locations 0 to 3: POSITION, COLOR, TEXCOORD 0, TEXCOORD 1 (xsdk docs/GUIDE.md, "Vertex inputs by location"). */
static const XsdkVertexElement layout[] = {
    { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
    { .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 8 },
    { .usage = XSDK_USAGE_TEXCOORD, .usage_index = 0, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
    { .usage = XSDK_USAGE_TEXCOORD, .usage_index = 1, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 32 },
};

static XsdkShader vertex_shader, pixel_shader;
static XsdkBlendState blend;
static XsdkDepthStencilState depth_stencil;
static XsdkRasterState raster;
static volatile uint32_t *ring;
static uint32_t ring_address;
static uint32_t block, shapes, fences[BLOCKS];
static uint32_t frame;
static float to_clip[4];
static bool ready;

bool gfx_init(void)
{
    NTSTATUS st = xsdk_shader_load(&vertex_shader, shape_vert.container, shape_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pixel_shader, shape_frag.container, shape_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&vertex_shader, layout, sizeof layout / sizeof layout[0],
                                           (const uint32_t[]){ STRIDE_BYTES });
    if (!NT_SUCCESS(st)) {
        printf("[E] gfx: loading shaders failed: 0x%08X\n", (unsigned)st);
        return false;
    }
    ring = xsdk_gpu_alloc(BLOCKS * BLOCK_BYTES);
    if (!ring) {
        printf("[E] gfx: no GPU memory for the shape ring\n");
        return false;
    }
    ring_address = xsdk_gpu_address(ring);
    /* Over what's drawn by the colour's alpha, as text is. */
    const XsdkBlend b = { .color_source = XSDK_BLEND_SRC_ALPHA,
                          .color_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
                          .color_op = XSDK_BLEND_OP_ADD,
                          .alpha_source = XSDK_BLEND_ONE,
                          .alpha_dest = XSDK_BLEND_ONE_MINUS_SRC_ALPHA,
                          .alpha_op = XSDK_BLEND_OP_ADD,
                          .write_mask = XSDK_WRITE_ALL };
    xsdk_gpu_blend_state(&blend, &b);
    xsdk_gpu_depth_stencil_state(&depth_stencil, &(const XsdkDepthStencil){ .depth_test = false });
    xsdk_gpu_raster_state(&raster, &(const XsdkRaster){ .cull = XSDK_CULL_NONE });
    ready = true;
    return true;
}

void gfx_shutdown(void)
{
    if (!ready)
        return;
    xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
    xsdk_gpu_free(ring);
    ring = NULL;
    xsdk_shader_free(&pixel_shader);
    xsdk_shader_free(&vertex_shader);
    ready = false;
}

void gfx_frame_begin(uint32_t width, uint32_t height)
{
    frame++;
    if (!ready)
        return;
    block = (block + 1) % BLOCKS;
    shapes = 0;
    if (fences[block]) {
        NTSTATUS st = xsdk_gpu_wait(fences[block], FENCE_TIMEOUT_MS);
        if (!NT_SUCCESS(st))
            printf("[W] gfx: fence wait failed: 0x%08X\n", (unsigned)st);
        fences[block] = 0;
    }
    to_clip[0] = 2.0f / (float)width;
    to_clip[1] = -2.0f / (float)height;
    to_clip[2] = -1.0f;
    to_clip[3] = 1.0f;
}

void gfx_frame_end(void)
{
    if (ready && shapes)
        fences[block] = xsdk_gpu_fence();
}

uint32_t gfx_frame(void)
{
    return frame;
}

static float channel(uint32_t v)
{
    return (float)(v & 0xFF) * (1.0f / 255.0f);
}

/* GPU memory takes volatile 32-bit stores (xsdk CLAUDE.md: GCC would merge plain ones into unaligned 64-bit ones). */
static void put_vertex(volatile uint32_t *dst, const float v[VERTEX_FLOATS])
{
    for (int i = 0; i < VERTEX_FLOATS; i++) {
        union {
            float f;
            uint32_t u;
        } c = { .f = v[i] };
        dst[i] = c.u;
    }
}

static void shape(float x, float y, float w, float h, float radius, float soft, uint32_t top, uint32_t bottom)
{
    if (!ready || w <= 0.0f || h <= 0.0f)
        return;
    if (shapes == SHAPES_PER_BLOCK) {
        static bool warned;
        if (!warned)
            printf("[W] gfx: more than %d shapes in a frame; the rest are skipped\n", SHAPES_PER_BLOCK);
        warned = true;
        return;
    }
    const float hw = w * 0.5f, hh = h * 0.5f;
    const float r = radius < 0.0f ? 0.0f : radius > hw ? hw : radius > hh ? hh : radius;
    const float edge = soft > 1.0f ? soft : 1.0f;
    const float cx = x + hw, cy = y + hh;
    const float x0 = x - edge, y0 = y - edge, x1 = x + w + edge, y1 = y + h + edge;
    const float corner[CORNERS][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
    volatile uint32_t *slot = &ring[(block * SHAPES_PER_BLOCK + shapes) * (SHAPE_BYTES / 4)];
    for (int i = 0; i < CORNERS; i++) {
        const uint32_t c = i < 2 ? top : bottom;
        const float px = corner[i][0], py = corner[i][1];
        /* Pixel centres sit on whole coordinates (D3D9's, as xsdk's text.c says), so edges go half a pixel up and
         * left; the local position stays on the shape's own grid. */
        const float v[VERTEX_FLOATS] = {
            px - 0.5f, py - 0.5f,
            channel(c >> 16), channel(c >> 8), channel(c), channel(c >> 24),
            px - cx, py - cy,
            hw, hh, r, 1.0f / edge,
        };
        put_vertex(slot + i * VERTEX_FLOATS, v);
    }
    const uint32_t address = ring_address + (block * SHAPES_PER_BLOCK + shapes) * SHAPE_BYTES;
    shapes++;
    xsdk_gpu_invalidate(address, SHAPE_BYTES);
    xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
    xsdk_built_shader_set_literals(&shape_vert);
    xsdk_built_shader_set_literals(&shape_frag);
    xsdk_gpu_set_vertex_constants(SHAPE_VERT_PUSH, to_clip, SHAPE_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_blend(&blend);
    xsdk_gpu_set_depth_stencil(&depth_stencil);
    xsdk_gpu_set_raster(&raster);
    xsdk_gpu_set_vertices(0, address, SHAPE_BYTES);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
}

void gfx_rect(float x, float y, float w, float h, float radius, uint32_t argb)
{
    shape(x, y, w, h, radius, 1.0f, argb, argb);
}

void gfx_rect_v(float x, float y, float w, float h, float radius, uint32_t top_argb, uint32_t bottom_argb)
{
    shape(x, y, w, h, radius, 1.0f, top_argb, bottom_argb);
}

void gfx_shadow(float x, float y, float w, float h, float radius, float blur, uint32_t argb)
{
    shape(x, y, w, h, radius, blur, argb, argb);
}

void gfx_circle(float cx, float cy, float r, uint32_t argb)
{
    shape(cx - r, cy - r, 2.0f * r, 2.0f * r, r, 1.0f, argb, argb);
}

/* ---- text ---- */

void gfx_text(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
    if (utf8 && utf8[0])
        xsdk_text_draw(x, y, scale, argb, utf8);
}

int32_t gfx_text_width(const char *utf8, uint32_t scale)
{
    uint32_t w = 0, h = 0;
    if (utf8 && utf8[0])
        xsdk_text_measure(utf8, scale, &w, &h);
    return (int32_t)w;
}

void gfx_text_center(int32_t center_x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
    gfx_text(center_x - gfx_text_width(utf8, scale) / 2, y, scale, argb, utf8);
}

void gfx_text_right(int32_t right, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
    gfx_text(right - gfx_text_width(utf8, scale), y, scale, argb, utf8);
}

static bool continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

void gfx_fit(char *out, size_t size, const char *in, int max_chars)
{
    int chars = 0;
    size_t i = 0;
    for (; in[i]; i++)
        if (!continuation((unsigned char)in[i]))
            chars++;
    if (chars <= max_chars) {
        snprintf(out, size, "%s", in);
        return;
    }
    /* Keep max_chars - 1 characters and an ellipsis (U+2026). */
    chars = 0;
    for (i = 0; in[i]; i++) {
        if (!continuation((unsigned char)in[i]) && ++chars == max_chars)
            break;
    }
    snprintf(out, size, "%.*s\xE2\x80\xA6", (int)i, in);
}

int gfx_wrap(char *out, size_t size, const char *in, int max_chars)
{
    size_t o = 0;
    int lines = 1, column = 0;
    if (!size)
        return 0;
    for (const char *p = in; *p && o + 1 < size;) {
        if (*p == '\n') {
            out[o++] = *p++;
            lines++;
            column = 0;
            continue;
        }
        if (*p == ' ' && column == 0) {   /* no space at a line's start */
            p++;
            continue;
        }
        /* The next word (or space) and its length in characters. */
        const char *end = p;
        int chars = 0;
        if (*end == ' ') {
            end++;
            chars = 1;
        } else {
            while (*end && *end != ' ' && *end != '\n') {
                if (!continuation((unsigned char)*end))
                    chars++;
                end++;
            }
        }
        if (column + chars > max_chars && column > 0) {
            if (*p == ' ') {
                p++;
                continue;   /* the break replaces the space */
            }
            out[o++] = '\n';
            lines++;
            column = 0;
            continue;
        }
        while (p < end && o + 1 < size)
            out[o++] = *p++;
        column += chars;
    }
    out[o] = 0;
    return lines;
}

/* ---- controller buttons ---- */

static const uint32_t BUTTON_A = 0xFF5DBB46, BUTTON_B = 0xFFE5483D, BUTTON_X = 0xFF3A8EE6, BUTTON_Y = 0xFFF2BE2C,
                      BUTTON_INK = 0xFF0B1018, PILL = 0xFF46526A, PILL_INK = 0xFFFFFFFF, DPAD = 0xFF9AA6B8;

int32_t gfx_button(int32_t x, int32_t center_y, char button)
{
    const float cy = (float)center_y;
    switch (button) {
    case 'A':
    case 'B':
    case 'X':
    case 'Y': {
        const uint32_t colour = button == 'A' ? BUTTON_A : button == 'B' ? BUTTON_B : button == 'X' ? BUTTON_X : BUTTON_Y;
        gfx_circle((float)x + 14.0f, cy, 14.0f, colour);
        const char letter[2] = { button, 0 };
        /* Twice, a pixel apart: a bolder letter. */
        gfx_text(x + 9, center_y - 10, 1, BUTTON_INK, letter);
        gfx_text(x + 10, center_y - 10, 1, BUTTON_INK, letter);
        return 28;
    }
    case 'L':
    case 'R':
    case 'K':
    case 'S': {
        const char *label = button == 'L' ? "LB" : button == 'R' ? "RB" : button == 'K' ? "BACK" : "START";
        const int32_t w = gfx_text_width(label, 1) + 18;
        gfx_rect((float)x, cy - 13.0f, (float)w, 26.0f, 13.0f, PILL);
        gfx_text(x + 9, center_y - 10, 1, PILL_INK, label);
        return w;
    }
    case 'D':
        gfx_rect((float)x, cy - 4.0f, 26.0f, 8.0f, 2.0f, DPAD);
        gfx_rect((float)x + 9.0f, cy - 13.0f, 8.0f, 26.0f, 2.0f, DPAD);
        return 26;
    default:
        return 0;
    }
}

int32_t gfx_hint(int32_t x, int32_t center_y, char button, const char *label, uint32_t argb)
{
    int32_t w = gfx_button(x, center_y, button);
    gfx_text(x + w + 8, center_y - 10, 1, argb, label);
    return w + 8 + gfx_text_width(label, 1) + 28;
}

void gfx_spinner(float cx, float cy, float r, uint32_t argb)
{
    const uint32_t head = (frame / 5) % 8;
    for (uint32_t i = 0; i < 8; i++) {
        const float angle = (float)i * (3.14159265f / 4.0f);
        const uint32_t behind = (head + 8 - i) % 8;   /* 0 for the brightest dot */
        const uint32_t alpha = ((argb >> 24) * (8 - behind)) / 8;
        gfx_circle(cx + r * sinf(angle), cy - r * cosf(angle), r * 0.2f, (argb & 0x00FFFFFF) | (alpha << 24));
    }
}

void gfx_logo(float x, float y, float size)
{
    static const uint32_t TILE = 0xFF4A7BFF, MOON = 0xFFFFFFFF;
    gfx_rect(x, y, size, size, size * 0.28f, TILE);
    const float cx = x + size * 0.47f, cy = y + size * 0.52f;
    gfx_circle(cx, cy, size * 0.30f, MOON);
    gfx_circle(cx + size * 0.13f, cy - size * 0.10f, size * 0.27f, TILE);
}
