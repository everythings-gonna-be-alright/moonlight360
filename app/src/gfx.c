#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "font_data.h"
#include "theme.h"
#include "moonlight360_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
    BLOCKS = 3,               /* frames of shapes in flight: one written while the GPU may still read the others */
    SHAPES_PER_BLOCK = 1024,
    CORNERS = 4,
    VERTEX_FLOATS = 12,       /* position 2, colour 4, local 2, shape 4 */
    STRIDE_BYTES = VERTEX_FLOATS * 4,
    SHAPE_BYTES = CORNERS * STRIDE_BYTES,   /* 192: a multiple of 64, as text.c's glyphs are */
    BLOCK_BYTES = SHAPES_PER_BLOCK * SHAPE_BYTES,
    INDICES_PER_SHAPE = 6,    /* two triangles of the four corners */
    /* Glyphs: quads as the shapes', with a position, a colour and an atlas coordinate a corner. */
    GLYPHS_PER_BLOCK = 2048,
    GLYPH_VERTEX_FLOATS = 8,
    GLYPH_STRIDE_BYTES = GLYPH_VERTEX_FLOATS * 4,
    GLYPH_BYTES = CORNERS * GLYPH_STRIDE_BYTES,   /* 128 */
    GLYPH_BLOCK_BYTES = GLYPHS_PER_BLOCK * GLYPH_BYTES,
    /* One index list serves both: quad i's corners are 4i to 4i + 3 either way. */
    INDEX_BYTES = GLYPHS_PER_BLOCK * INDICES_PER_SHAPE * 2,   /* 16-bit */
    /* The allocation: the shapes' blocks, the glyphs' blocks, the indices. */
    GLYPHS_OFFSET = BLOCKS * BLOCK_BYTES,
    INDEX_OFFSET = GLYPHS_OFFSET + BLOCKS * GLYPH_BLOCK_BYTES,
    LINE_HEIGHT = 20,         /* a line at scale 1, as the old bitmap font's cell */
    FENCE_TIMEOUT_MS = 500,
};
_Static_assert(GLYPHS_PER_BLOCK >= SHAPES_PER_BLOCK, "the index list covers both");

/* Locations 0 to 3: POSITION, COLOR, TEXCOORD 0, TEXCOORD 1 (xsdk docs/GUIDE.md, "Vertex inputs by location"). */
static const XsdkVertexElement layout[] = {
    { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
    { .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 8 },
    { .usage = XSDK_USAGE_TEXCOORD, .usage_index = 0, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
    { .usage = XSDK_USAGE_TEXCOORD, .usage_index = 1, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 32 },
};

static const XsdkVertexElement glyph_layout[] = {
    { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
    { .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 8 },
    { .usage = XSDK_USAGE_TEXCOORD, .usage_index = 0, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 24 },
};

static XsdkShader vertex_shader, pixel_shader, font_vertex_shader, font_pixel_shader, image_vertex_shader,
    image_pixel_shader;
static XsdkTexture atlas;
static XsdkSamplerState atlas_sampler;
static uint32_t glyphs, glyphs_drawn;   /* this block's glyphs written, and those already drawn */
static XsdkBlendState blend;
static XsdkDepthStencilState depth_stencil;
static XsdkRasterState raster;
static volatile uint32_t *ring;
static uint32_t ring_address;
static uint32_t index_address;   /* after the blocks, in the same allocation */
static uint32_t block, shapes, drawn, fences[BLOCKS];   /* drawn: the shapes of this block already drawn */
static uint32_t frame;
static float to_clip[4];
static bool ready;

/* The font's atlas (font_data.h, 4 bits a texel) as an 8-bit texture, filtered: glyphs drawn 1:1 land on texel
 * centres, and the big sizes' missing characters, stretched from the next size down, come out smooth. */
static bool make_atlas(void)
{
    NTSTATUS st = xsdk_texture_create(&atlas, XSDK_FORMAT_R8, FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT, 1, true);
    if (!NT_SUCCESS(st)) {
        printf("[E] gfx: the font's texture failed: 0x%08X\n", (unsigned)st);
        return false;
    }
    uint8_t *texels = malloc(FONT_ATLAS_WIDTH * FONT_ATLAS_HEIGHT);
    if (!texels) {
        printf("[E] gfx: no memory for the font\n");
        return false;
    }
    for (uint32_t i = 0; i < FONT_ATLAS_WIDTH * FONT_ATLAS_HEIGHT / 2; i++) {
        texels[2 * i] = (uint8_t)((font_atlas[i] >> 4) * 17);
        texels[2 * i + 1] = (uint8_t)((font_atlas[i] & 0xF) * 17);
    }
    xsdk_texture_upload(&atlas, 0, texels, FONT_ATLAS_WIDTH);
    free(texels);
    xsdk_gpu_invalidate(atlas.address, atlas.size_bytes);
    const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
                                  .min_filter = XSDK_FILTER_LINEAR,
                                  .mip_filter = XSDK_FILTER_POINT,
                                  .address_u = XSDK_ADDRESS_CLAMP,
                                  .address_v = XSDK_ADDRESS_CLAMP,
                                  .address_w = XSDK_ADDRESS_CLAMP };
    xsdk_texture_sampler_state(&atlas_sampler, &sampler);
    return true;
}

bool gfx_init(void)
{
    NTSTATUS st = xsdk_shader_load(&vertex_shader, shape_vert.container, shape_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pixel_shader, shape_frag.container, shape_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&vertex_shader, layout, sizeof layout / sizeof layout[0],
                                           (const uint32_t[]){ STRIDE_BYTES });
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&font_vertex_shader, font_vert.container, font_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&font_pixel_shader, font_frag.container, font_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&font_vertex_shader, glyph_layout,
                                           sizeof glyph_layout / sizeof glyph_layout[0],
                                           (const uint32_t[]){ GLYPH_STRIDE_BYTES });
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&image_vertex_shader, image_vert.container, image_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&image_pixel_shader, image_frag.container, image_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&image_vertex_shader, glyph_layout,
                                           sizeof glyph_layout / sizeof glyph_layout[0],
                                           (const uint32_t[]){ GLYPH_STRIDE_BYTES });
    if (!NT_SUCCESS(st)) {
        printf("[E] gfx: loading shaders failed: 0x%08X\n", (unsigned)st);
        return false;
    }
    ring = xsdk_gpu_alloc(INDEX_OFFSET + INDEX_BYTES);
    if (!ring) {
        printf("[E] gfx: no GPU memory for the shape ring\n");
        return false;
    }
    ring_address = xsdk_gpu_address(ring);
    /* Corners top left, top right, bottom left, bottom right, as the strip had them: shape i's two triangles are
     * 4i + (0, 1, 2) and 4i + (2, 1, 3). Two 16-bit big-endian indices a word. */
    volatile uint32_t *indices = ring + INDEX_OFFSET / 4;
    static const uint16_t corners[INDICES_PER_SHAPE] = { 0, 1, 2, 2, 1, 3 };
    for (uint32_t i = 0; i < GLYPHS_PER_BLOCK * INDICES_PER_SHAPE; i += 2) {
        const uint32_t first = (i / INDICES_PER_SHAPE) * CORNERS + corners[i % INDICES_PER_SHAPE];
        const uint32_t second = ((i + 1) / INDICES_PER_SHAPE) * CORNERS + corners[(i + 1) % INDICES_PER_SHAPE];
        indices[i / 2] = first << 16 | second;
    }
    index_address = ring_address + INDEX_OFFSET;
    xsdk_gpu_invalidate(index_address, INDEX_BYTES);
    if (!make_atlas())
        return false;
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
    xsdk_texture_free(&atlas);
    xsdk_shader_free(&font_pixel_shader);
    xsdk_shader_free(&font_vertex_shader);
    xsdk_shader_free(&image_pixel_shader);
    xsdk_shader_free(&image_vertex_shader);
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
    shapes = drawn = glyphs = glyphs_drawn = 0;
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

/* The shapes written since the last flush, in one draw: the state once and an indexed list, not a draw each. */
static void flush_shapes(void)
{
    if (!ready || drawn == shapes)
        return;
    const uint32_t count = shapes - drawn;
    const uint32_t address = ring_address + (block * SHAPES_PER_BLOCK + drawn) * SHAPE_BYTES;
    drawn = shapes;
    xsdk_gpu_invalidate(address, count * SHAPE_BYTES);
    xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
    xsdk_built_shader_set_literals(&shape_vert);
    xsdk_built_shader_set_literals(&shape_frag);
    xsdk_gpu_set_vertex_constants(SHAPE_VERT_PUSH, to_clip, SHAPE_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_blend(&blend);
    xsdk_gpu_set_depth_stencil(&depth_stencil);
    xsdk_gpu_set_raster(&raster);
    xsdk_gpu_set_vertices(0, address, count * SHAPE_BYTES);
    xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, count * INDICES_PER_SHAPE, false);
}

/* The same for the glyphs written since the last flush. */
static void flush_glyphs(void)
{
    if (!ready || glyphs_drawn == glyphs)
        return;
    const uint32_t count = glyphs - glyphs_drawn;
    const uint32_t address = ring_address + GLYPHS_OFFSET + (block * GLYPHS_PER_BLOCK + glyphs_drawn) * GLYPH_BYTES;
    glyphs_drawn = glyphs;
    xsdk_gpu_invalidate(address, count * GLYPH_BYTES);
    xsdk_gpu_set_shaders(&font_vertex_shader, &font_pixel_shader);
    xsdk_built_shader_set_literals(&font_vert);
    xsdk_built_shader_set_literals(&font_frag);
    xsdk_gpu_set_vertex_constants(FONT_VERT_PUSH, to_clip, FONT_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_texture(FONT_FRAG_SAMPLER_0_0, &atlas, &atlas_sampler);
    xsdk_gpu_set_blend(&blend);
    xsdk_gpu_set_depth_stencil(&depth_stencil);
    xsdk_gpu_set_raster(&raster);
    xsdk_gpu_set_vertices(0, address, count * GLYPH_BYTES);
    xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, count * INDICES_PER_SHAPE, false);
}

/* Shapes and glyphs are drawn in the order they came: only one kind waits at a time. */
void gfx_flush(void)
{
    flush_shapes();
    flush_glyphs();
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

/* gfx_scale_begin's scale and the point it scales about; 1 when not scaling. */
/* gfx_transform_begin's: a point p is drawn at to + (p - from) * zoom; off, the identity. */
static float zoom = 1.0f, zoom_x, zoom_y, zoom_tx, zoom_ty;
static bool transformed;

void gfx_transform_begin(float scale, float from_x, float from_y, float to_x, float to_y)
{
    transformed = true;
    zoom = scale;
    zoom_x = from_x;
    zoom_y = from_y;
    zoom_tx = to_x;
    zoom_ty = to_y;
}

void gfx_scale_begin(float scale, float origin_x, float origin_y)
{
    gfx_transform_begin(scale, origin_x, origin_y, origin_x, origin_y);
}

void gfx_scale_end(void)
{
    transformed = false;
    zoom = 1.0f;
}

static void shape(float x, float y, float w, float h, float radius, float soft, uint32_t top, uint32_t bottom)
{
    if (transformed) {
        x = zoom_tx + (x - zoom_x) * zoom;
        y = zoom_ty + (y - zoom_y) * zoom;
        w *= zoom;
        h *= zoom;
        radius *= zoom;
    }
    if (!ready || w <= 0.0f || h <= 0.0f)
        return;
    flush_glyphs();   /* text before it stays under it */
    if (shapes == SHAPES_PER_BLOCK) {
        flush_shapes();
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
    shapes++;   /* drawn at the next flush: before text, at the frame's end */
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

/* ---- text: Inter, anti-aliased (font_data.h, made by dev/fonts/gen_font.py) ---- */

static uint32_t next_point(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t point = p[0];
    int extra = point >= 0xF0 ? 3 : point >= 0xE0 ? 2 : point >= 0xC0 ? 1 : 0;
    point &= extra ? 0x3Fu >> extra : 0xFFu;
    int i = 1;
    for (; i <= extra && (p[i] & 0xC0) == 0x80; i++)
        point = point << 6 | (p[i] & 0x3Fu);
    *text = (const char *)p + i;
    return i == extra + 1 ? point : '?';
}

/* The glyph for point at size (0 to FONT_SIZES - 1), or NULL. ASCII is the first 95 of every size. */
static const FontGlyph *find_glyph(int size, uint32_t point)
{
    const FontGlyph *g = font_sizes[size].glyphs;
    if (point >= 0x20 && point < 0x7F && font_sizes[size].glyph_count >= 0x7F - 0x20)   /* sizes with all of ASCII */
        return &g[point - 0x20];
    int lo = 0, hi = font_sizes[size].glyph_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g[mid].point == point)
            return &g[mid];
        if (g[mid].point < point)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* The glyph to draw point with at scale, and how much to stretch it: a big size lacking it borrows size 2's. */
/* font_data.h's last sizes: scale 1 at 80% and at 70% (gfx_scale_begin), and the buttons' bold letters. */
enum { SIZE_80 = 4, SIZE_70 = 5, SIZE_BUTTON = 6 };
_Static_assert(FONT_SIZES == 7, "gen_font.py's sizes: 1, 2, 3, 4, 0.8, 0.7, the buttons' letters");

static const FontGlyph *glyph_for(uint32_t scale, uint32_t point, float *stretch)
{
    /* Scaled to 80% or 70%, scale 1 takes that size's own glyphs; anything else is its size's glyphs stretched. */
    const int small = scale != 1 ? -1 : zoom > 0.79f && zoom < 0.81f ? SIZE_80 : zoom > 0.69f && zoom < 0.71f ? SIZE_70 : -1;
    const int size = small >= 0 ? small : (scale < 1 ? 1 : scale > 4 ? 4 : (int)scale) - 1;
    *stretch = small >= 0 ? 1.0f : zoom;
    const FontGlyph *g = find_glyph(size, point);
    if (!g && size > 1 && size < SIZE_80) {
        g = find_glyph(1, point);
        *stretch = zoom * (float)(size + 1) / 2.0f;
    }
    if (!g)
        g = find_glyph(size, '?');
    return g;
}

/* A textured quad in the glyphs' ring: (x, y) to (x + w, y + h) in pixels showing the texture from (u0, v0) to (u1, v1),
 * in colour. False if the ring's block is full. */
static bool put_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, const float colour[4])
{
    if (glyphs == GLYPHS_PER_BLOCK) {
        static bool warned;
        if (!warned)
            printf("[W] gfx: more than %d glyphs in a frame; the rest are skipped\n", GLYPHS_PER_BLOCK);
        warned = true;
        return false;
    }
    /* Pixel centres sit on whole coordinates (D3D9's), so edges go half a pixel up and left. */
    const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x0 + w, y1 = y0 + h;
    const float corner[CORNERS][4] = { { x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x0, y1, u0, v1 }, { x1, y1, u1, v1 } };
    volatile uint32_t *slot =
        &ring[(GLYPHS_OFFSET + (block * GLYPHS_PER_BLOCK + glyphs) * GLYPH_BYTES) / 4];
    for (int i = 0; i < CORNERS; i++) {
        const float v[GLYPH_VERTEX_FLOATS] = { corner[i][0], corner[i][1], colour[0], colour[1],
                                               colour[2],    colour[3],    corner[i][2], corner[i][3] };
        for (int k = 0; k < GLYPH_VERTEX_FLOATS; k++) {
            union {
                float f;
                uint32_t u;
            } c = { .f = v[k] };
            slot[i * GLYPH_VERTEX_FLOATS + k] = c.u;
        }
    }
    glyphs++;
    return true;
}

static void put_glyph(const FontGlyph *g, float x, float y, float stretch, const float colour[4])
{
    /* Drawn at the next flush: before a shape, at the frame's end. */
    put_quad(x, y, g->w * stretch, g->h * stretch, (float)g->x / FONT_ATLAS_WIDTH, (float)g->y / FONT_ATLAS_HEIGHT,
             (float)(g->x + g->w) / FONT_ATLAS_WIDTH, (float)(g->y + g->h) / FONT_ATLAS_HEIGHT, colour);
}

/* ---- images: an RGBA texture each, drawn at once (a draw each, between the shapes and text around them) ---- */

struct GfxImage {
    XsdkTexture texture;
};

GfxImage *gfx_image_create(const uint8_t *rgba, int width, int height)
{
    if (!ready || !rgba)
        return NULL;
    GfxImage *image = calloc(1, sizeof *image);
    if (!image)
        return NULL;
    const NTSTATUS st = xsdk_texture_create(&image->texture, XSDK_FORMAT_R8G8B8A8, (uint32_t)width, (uint32_t)height,
                                            1, true);
    if (!NT_SUCCESS(st)) {
        printf("[W] gfx: an image's texture failed: 0x%08X\n", (unsigned)st);
        free(image);
        return NULL;
    }
    xsdk_texture_upload(&image->texture, 0, rgba, (uint32_t)width * 4);
    xsdk_gpu_invalidate(image->texture.address, image->texture.size_bytes);
    return image;
}

GfxImage *gfx_image_create_rounded(const uint8_t *rgba, int width, int height, float radius)
{
    if (!rgba || width <= 0 || height <= 0)
        return NULL;
    uint8_t *copy = malloc((size_t)width * height * 4);
    if (!copy)
        return gfx_image_create(rgba, width, height);
    memcpy(copy, rgba, (size_t)width * height * 4);
    /* Each corner's square: alpha from the pixel centre's distance past the corner's circle, a pixel's width of
     * fade, so the edge is smooth. */
    const int r = (int)ceilf(radius);
    for (int y = 0; y < r && y < height; y++)
        for (int x = 0; x < r && x < width; x++) {
            const float dx = radius - ((float)x + 0.5f), dy = radius - ((float)y + 0.5f);
            const float past = sqrtf(dx * dx + dy * dy) - radius + 0.5f;
            if (past <= 0.0f)
                continue;
            const float keep = past >= 1.0f ? 0.0f : 1.0f - past;
            const int xs[2] = { x, width - 1 - x }, ys[2] = { y, height - 1 - y };
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 2; b++) {
                    uint8_t *alpha = &copy[((size_t)ys[b] * width + xs[a]) * 4 + 3];
                    *alpha = (uint8_t)(*alpha * keep + 0.5f);
                }
        }
    GfxImage *image = gfx_image_create(copy, width, height);
    free(copy);
    return image;
}

void gfx_image_free(GfxImage *image)
{
    if (!image)
        return;
    xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);   /* no draw still reading it */
    xsdk_texture_free(&image->texture);
    free(image);
}

void gfx_image(const GfxImage *image, float x, float y, float w, float h, uint32_t argb)
{
    if (!ready || !image)
        return;
    flush_shapes();
    flush_glyphs();
    const float colour[4] = { channel(argb >> 16), channel(argb >> 8), channel(argb), channel(argb >> 24) };
    if (!put_quad(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, colour))
        return;
    glyphs_drawn = glyphs;   /* this quad is drawn here, not with the text */
    const uint32_t address = ring_address + GLYPHS_OFFSET + (block * GLYPHS_PER_BLOCK + glyphs - 1) * GLYPH_BYTES;
    xsdk_gpu_invalidate(address, GLYPH_BYTES);
    xsdk_gpu_set_shaders(&image_vertex_shader, &image_pixel_shader);
    xsdk_built_shader_set_literals(&image_vert);
    xsdk_built_shader_set_literals(&image_frag);
    xsdk_gpu_set_vertex_constants(IMAGE_VERT_PUSH, to_clip, IMAGE_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_texture(IMAGE_FRAG_SAMPLER_0_0, &image->texture, &atlas_sampler);
    xsdk_gpu_set_blend(&blend);
    xsdk_gpu_set_depth_stencil(&depth_stencil);
    xsdk_gpu_set_raster(&raster);
    xsdk_gpu_set_vertices(0, address, GLYPH_BYTES);
    xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, INDICES_PER_SHAPE, false);
}

void gfx_text(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
    if (!ready || !utf8 || !utf8[0])
        return;
    flush_shapes();   /* the shapes before it stay under it */
    const float colour[4] = { channel(argb >> 16), channel(argb >> 8), channel(argb), channel(argb >> 24) };
    const float left = transformed ? zoom_tx + ((float)x - zoom_x) * zoom : (float)x;
    float pen = left, top = transformed ? zoom_ty + ((float)y - zoom_y) * zoom : (float)y;
    top = floorf(top + 0.5f);   /* whole pixels, as the pen's glyphs are: sharp */
    while (*utf8) {
        const uint32_t point = next_point(&utf8);
        if (point == '\n') {
            pen = left;
            top += floorf((float)(LINE_HEIGHT * scale) * zoom + 0.5f);
            continue;
        }
        if (point < 0x20)
            continue;
        float stretch;
        const FontGlyph *g = glyph_for(scale, point, &stretch);
        if (!g)
            continue;
        if (g->w)   /* whole pixels: 1:1 glyphs stay sharp */
            put_glyph(g, floorf(pen + g->left * stretch + 0.5f), top + g->top * stretch, stretch, colour);
        pen += g->advance * stretch / 64.0f;
    }
}

void gfx_icon(int index, float x, float y, uint32_t argb)
{
    if (!ready || index < 0 || index >= FONT_ICONS)
        return;
    flush_shapes();   /* the shapes before it stay under it */
    const float colour[4] = { channel(argb >> 16), channel(argb >> 8), channel(argb), channel(argb >> 24) };
    const FontGlyph *g = &font_icons[index];
    put_glyph(g, floorf(x + 0.5f) + (float)g->left, floorf(y + 0.5f) + (float)g->top, 1.0f, colour);
}

int32_t gfx_text_width(const char *utf8, uint32_t scale)
{
    if (!utf8)
        return 0;
    /* In the caller's units (unscaled, as the positions it lays out are): what's drawn, measured with the glyphs that
     * draw it, over the scale. */
    float line = 0.0f, longest = 0.0f;
    while (*utf8) {
        const uint32_t point = next_point(&utf8);
        if (point == '\n') {
            line = 0.0f;
            continue;
        }
        float stretch;
        const FontGlyph *g = point < 0x20 ? NULL : glyph_for(scale, point, &stretch);
        if (g)
            line += g->advance * stretch / 64.0f;
        longest = line > longest ? line : longest;
    }
    return (int32_t)ceilf(longest / zoom);
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

void gfx_fit_width(char *out, size_t size, const char *in, uint32_t scale, int32_t max_width)
{
    snprintf(out, size, "%s", in);
    if (gfx_text_width(out, scale) <= max_width)
        return;
    /* Shorter by a character at a time (whole UTF-8 sequences), with an ellipsis, until it fits. */
    size_t n = strlen(out);
    char trial[512];
    while (n > 0) {
        do
            n--;
        while (n > 0 && continuation((unsigned char)out[n]));
        while (n > 0 && out[n - 1] == ' ')
            n--;
        snprintf(trial, sizeof trial, "%.*s\xE2\x80\xA6", (int)n, out);
        if (gfx_text_width(trial, scale) <= max_width)
            break;
    }
    snprintf(out, size, "%s", trial);
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
                      BUTTON_INK = 0xFF121212, PILL = 0xFF3A3A40, PILL_INK = 0xFFF4F4F5, DPAD = 0xFFA6A6AD;

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
        /* The letter, bold, its ink centred on the circle, whole pixels. */
        const FontGlyph *g = find_glyph(SIZE_BUTTON, (uint32_t)button);
        if (g && ready) {
            flush_shapes();
            const float colour[4] = { channel(BUTTON_INK >> 16), channel(BUTTON_INK >> 8), channel(BUTTON_INK),
                                      channel(BUTTON_INK >> 24) };
            put_glyph(g, floorf((float)x + 14.0f - g->w / 2.0f + 0.5f), floorf(cy - g->h / 2.0f + 0.5f), 1.0f, colour);
        }
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

/* The mark: an orange tile with a slim crescent moon cut out of it, tilted as the moon is seen. Flat shapes only. */
void gfx_logo(float x, float y, float size)
{
    const uint32_t TILE = THEME_ACCENT, MOON = 0xFF121212;   /* the tile takes the theme's accent */
    const float s = size;
    gfx_rect(x, y, s, s, s * 0.28f, TILE);
    /* The moon, then the tile's colour over its upper right: what's left is a crescent opening that way. */
    const float cx = x + s * 0.53f, cy = y + s * 0.50f, r = s * 0.29f;   /* optically centred: the crescent sits low left */
    gfx_circle(cx, cy, r, MOON);
    gfx_circle(cx + s * 0.12f, cy - s * 0.11f, r * 0.88f, TILE);
}
