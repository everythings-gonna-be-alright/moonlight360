#include "gpumc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The build's shader header: moonlight360_shaders.h in the app, the bench's own there (GPUMC_SHADERS). */
#ifdef GPUMC_SHADERS
#include GPUMC_SHADERS
#else
#include "moonlight360_shaders.h"
#endif
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
    PLANES = 3,
    VERTEX_FLOATS = 4,   /* position, pixel */
    CORNERS = 4,
    QUAD_BYTES = CORNERS * VERTEX_FLOATS * 4,
    FENCE_TIMEOUT_MS = 1000,
    LINE = 128,          /* the CPU's cache line */
    PROBE_W = 32,
    PROBE_H = 8,
};

static const XsdkVertexElement layout[] = {
    { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
    { .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 8 },
};

static XsdkShader vertex_shader, chroma_shader, probe_shader, copy_shader;
/* The multi-pass luma: planes G, B, H and J of the reference (margins of a texel left and right, 4 rows above and
 * below), then the final pass picking each block's quarter position from them. */
static XsdkShader pg_shader, pb_shader, ph_shader, pj_shader, final_shader;
static XsdkTexture planes[4];
/* Residuals the CPU writes while parsing (h264_xenon.c), as 16-bit signed textures of four samples a texel over the
 * same memory, and a byte a macroblock flagging the ones that have one. */
static XsdkTexture res_tex[3], flags_tex;
static uint8_t *res_memory[3], *flags_memory;
/* Their tiled layout as a table (h264_xenon.h, xenon_hybrid_set_residual_layout): the first 32 rows' texel offsets
 * and how far each further band of 32 rows is. Tiled because the texture cache is: read linear, they cost the final
 * pass 9 ms a 1080p frame (hybench, 2026-11-23). */
static uint32_t *res_offsets[3], res_texels[3], res_band[3];
static int plane_w, plane_h;   /* texels, rows */
enum { PLANE_G, PLANE_B, PLANE_H, PLANE_J };
static bool hung;   /* the GPU stopped finishing work: nothing more is sent */
static XsdkSamplerState point_clamp;
static XsdkTexture grid_texture, out[PLANES], probe_out;
static uint8_t *grid_memory, *out_memory[PLANES], *probe_memory;
static volatile uint32_t *quads;    /* luma target's quad, chroma target's, probe's */
static int width, height;
static float order[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static bool ready;

/* libxsdk's (src/gpu_private.h): a texel's byte offset in Xenos's tiled layout, the pitch a multiple of 32. */
uint32_t xsdk_tiled_2d(uint32_t x, uint32_t y, uint32_t pitch_texels, uint32_t bytes_per_texel_log2);

static void build_untile_tables(void);

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

/* Memory the CPU caches and the GPU reads and writes: the CPU writes it out (dcbst) before the GPU reads, and drops
 * its copy (dcbf) before reading what the GPU wrote. */
static uint8_t *cached_physical(uint32_t bytes)
{
    const uint32_t page = 64 * 1024;
    return MmAllocatePhysicalMemoryEx(0, (bytes + page - 1) & ~(page - 1), PAGE_READWRITE | MEM_LARGE_PAGES, 0,
                                      UINT32_MAX, 4096);
}

static void write_out(const void *p, size_t bytes)
{
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)(LINE - 1); a < (uintptr_t)p + bytes; a += LINE)
        __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static void drop(const void *p, size_t bytes)
{
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)(LINE - 1); a < (uintptr_t)p + bytes; a += LINE)
        __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static void put_quad(int index, float w, float h)
{
    /* Edges half a pixel up and left (pixel centres on whole coordinates, as xsdk's text.c says); the pixel attribute
     * then reaches each centre as column + 0.5, row + 0.5. */
    const float v[CORNERS][VERTEX_FLOATS] = {
        { -0.5f, -0.5f, 0.0f, 0.0f },
        { w - 0.5f, -0.5f, w, 0.0f },
        { -0.5f, h - 0.5f, 0.0f, h },
        { w - 0.5f, h - 0.5f, w, h },
    };
    volatile uint32_t *dst = quads + index * (QUAD_BYTES / 4);
    for (int i = 0; i < CORNERS * VERTEX_FLOATS; i++) {
        union {
            float f;
            uint32_t u;
        } c = { .f = v[i / VERTEX_FLOATS][i % VERTEX_FLOATS] };
        dst[i] = c.u;
    }
}

static bool tiled_target(XsdkTexture *t, uint8_t **memory, int w, int h)
{
    if (!NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_R8G8B8A8, (uint32_t)w, (uint32_t)h, 1, true)))
        return false;
    /* A small one's base would sit in the packed mip tail, where a resolve can't write (hybench, 2026-11-23). */
    xsdk_texture_base_at_corner(t);
    *memory = cached_physical(t->size_bytes);
    if (!*memory)
        return false;
    xsdk_texture_attach(t, (volatile uint32_t *)*memory);
    return true;
}

bool gpumc_init(int coded_width, int coded_height)
{
    if (ready)
        return true;
    width = coded_width;
    height = coded_height;
    NTSTATUS st = xsdk_shader_load(&vertex_shader, mc_vert.container, mc_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&chroma_shader, mc_chroma_frag.container, mc_chroma_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&probe_shader, mc_probe_frag.container, mc_probe_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&copy_shader, mc_copy_frag.container, mc_copy_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pg_shader, mc_pg_frag.container, mc_pg_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pb_shader, mc_pb_frag.container, mc_pb_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&ph_shader, mc_ph_frag.container, mc_ph_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pj_shader, mc_pj_frag.container, mc_pj_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&final_shader, mc_final_frag.container, mc_final_frag.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&vertex_shader, layout, 2, (const uint32_t[]){ VERTEX_FLOATS * 4 });
    if (!NT_SUCCESS(st)) {
        printf("[E] gpumc: shaders: 0x%08X\n", (unsigned)st);
        return false;
    }
    const XsdkSampler s = {
        .mag_filter = XSDK_FILTER_POINT,
        .min_filter = XSDK_FILTER_POINT,
        .mip_filter = XSDK_FILTER_POINT,
        .address_u = XSDK_ADDRESS_CLAMP,
        .address_v = XSDK_ADDRESS_CLAMP,
    };
    xsdk_texture_sampler_state(&point_clamp, &s);

    if (!NT_SUCCESS(xsdk_texture_init(&grid_texture, XSDK_FORMAT_R32F, (uint32_t)width / 4, (uint32_t)height / 4, 1,
                                      false)) ||
        !(grid_memory = cached_physical(grid_texture.size_bytes))) {
        printf("[E] gpumc: no grid\n");
        return false;
    }
    xsdk_texture_attach(&grid_texture, (volatile uint32_t *)grid_memory);
    memset(grid_memory, 0, grid_texture.size_bytes);
    write_out(grid_memory, grid_texture.size_bytes);

    if (!tiled_target(&out[0], &out_memory[0], width / 4, height) ||
        !tiled_target(&out[1], &out_memory[1], width / 8, height / 2) ||
        !tiled_target(&out[2], &out_memory[2], width / 8, height / 2) ||
        !tiled_target(&probe_out, &probe_memory, PROBE_W, PROBE_H)) {
        printf("[E] gpumc: no output textures\n");
        return false;
    }
    static const uint32_t signed4[4] = { XSDK_SIGN_SIGNED, XSDK_SIGN_SIGNED, XSDK_SIGN_SIGNED, XSDK_SIGN_SIGNED };
    for (int p = 0; p < PLANES; p++) {
        XsdkTexture *t = &res_tex[p];
        if (!NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_R16G16B16A16, (uint32_t)(p ? width / 8 : width / 4),
                                          (uint32_t)(p ? height / 2 : height), 1, true)))
            return false;
        xsdk_texture_base_at_corner(t);
        res_texels[p] = t->width;
        res_offsets[p] = malloc(32 * t->width * sizeof(uint32_t));
        if (!(res_memory[p] = cached_physical(t->size_bytes)) || !res_offsets[p]) {
            printf("[E] gpumc: no residual textures\n");
            return false;
        }
        xsdk_texture_attach(t, (volatile uint32_t *)res_memory[p]);
        xsdk_texture_signs(t, signed4);
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < t->width; x++)
                res_offsets[p][y * t->width + x] = xsdk_tiled_2d(x, y, t->pitch, 3);
        res_band[p] = xsdk_tiled_2d(0, 32, t->pitch, 3) - xsdk_tiled_2d(0, 0, t->pitch, 3);
        /* The table's premise, checked against the SDK: a band further down is the same plus res_band. */
        for (uint32_t y = 32; y < t->height; y += 11)
            for (uint32_t x = 0; x < t->width; x += 7)
                if (xsdk_tiled_2d(x, y, t->pitch, 3) != res_offsets[p][(y % 32) * t->width + x] + (y / 32) * res_band[p]) {
                    printf("[E] gpumc: the residual's tiling isn't as the table expects\n");
                    return false;
                }
    }
    if (!NT_SUCCESS(xsdk_texture_init(&flags_tex, XSDK_FORMAT_R8, (uint32_t)width / 16, (uint32_t)height / 16, 1,
                                      false)) ||
        !(flags_memory = cached_physical(flags_tex.size_bytes))) {
        printf("[E] gpumc: no residual flags\n");
        return false;
    }
    xsdk_texture_attach(&flags_tex, (volatile uint32_t *)flags_memory);
    memset(flags_memory, 0, flags_tex.size_bytes);
    plane_w = width / 4 + 2;
    plane_h = height + 8;
    for (int i = 0; i < 4; i++)
        if (!NT_SUCCESS(xsdk_texture_create(&planes[i], XSDK_FORMAT_R8G8B8A8, (uint32_t)plane_w, (uint32_t)plane_h, 1,
                                            true))) {
            printf("[E] gpumc: no plane textures\n");
            return false;
        }
    quads = xsdk_gpu_alloc(4 * QUAD_BYTES);
    if (!quads)
        return false;
    put_quad(0, (float)width / 4, (float)height);
    put_quad(1, (float)width / 8, (float)height / 2);
    put_quad(2, PROBE_W, PROBE_H);
    put_quad(3, (float)plane_w, (float)plane_h);
    xsdk_gpu_invalidate(xsdk_gpu_address(quads), 4 * QUAD_BYTES);
    build_untile_tables();
    ready = true;
    printf("[I] gpumc: %dx%d, grid %ux%u (pitch %u)\n", width, height, (unsigned)grid_texture.width,
           (unsigned)grid_texture.height, (unsigned)grid_texture.pitch);
    return true;
}

void gpumc_shutdown(void)
{
    if (!ready)
        return;
    xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
    for (int p = 0; p < PLANES; p++)
        MmFreePhysicalMemory(0, out_memory[p]);
    MmFreePhysicalMemory(0, probe_memory);
    MmFreePhysicalMemory(0, grid_memory);
    xsdk_gpu_free(quads);
    for (int i = 0; i < 4; i++)
        xsdk_texture_free(&planes[i]);
    for (int p = 0; p < PLANES; p++) {
        MmFreePhysicalMemory(0, res_memory[p]);
        free(res_offsets[p]);
        res_offsets[p] = NULL;
    }
    MmFreePhysicalMemory(0, flags_memory);
    xsdk_shader_free(&final_shader);
    xsdk_shader_free(&pj_shader);
    xsdk_shader_free(&ph_shader);
    xsdk_shader_free(&pb_shader);
    xsdk_shader_free(&pg_shader);
    xsdk_shader_free(&copy_shader);
    xsdk_shader_free(&probe_shader);
    xsdk_shader_free(&chroma_shader);
    xsdk_shader_free(&vertex_shader);
    ready = false;
}

void gpumc_residual_layout(const uint32_t *offsets[3], uint32_t texels[3], uint32_t band[3])
{
    for (int p = 0; p < PLANES; p++) {
        offsets[p] = res_offsets[p];
        texels[p] = res_texels[p];
        band[p] = res_band[p];
    }
}

int gpumc_residual_sample(int p, int x, int y)
{
    const uint8_t *texel = res_memory[p] + res_offsets[p][(y & 31) * res_texels[p] + (x >> 2)] + (uint32_t)(y >> 5) * res_band[p];
    return ((const int16_t *)texel)[x & 3];
}

void gpumc_residual(int16_t *planes_out[3], int pitch[2], uint8_t **flags, int *flags_pitch)
{
    for (int p = 0; p < PLANES; p++)
        planes_out[p] = (int16_t *)res_memory[p];
    pitch[0] = (int)res_tex[0].pitch * 4;   /* samples: four a texel */
    pitch[1] = (int)res_tex[1].pitch * 4;
    *flags = flags_memory;
    *flags_pitch = (int)flags_tex.pitch;
}

uint32_t *gpumc_grid(int *pitch_blocks, int *rows_blocks)
{
    *pitch_blocks = (int)grid_texture.pitch;
    *rows_blocks = (int)grid_texture.height;
    return (uint32_t *)grid_memory;
}

/* A pass with any textures: slots[i] gets textures[i]; constants go to the pixel shader's push constants. */
static void draw(const XsdkShader *pixel, const XsdkBuiltShader *built, int push, int push_registers,
                 const float *constants, int count, const int *slots, const XsdkTexture *const *textures, int quad,
                 uint32_t w, uint32_t h, const XsdkTexture *dst)
{
    xsdk_gpu_set_target(w, h);
    xsdk_gpu_set_shaders(&vertex_shader, pixel);
    xsdk_built_shader_set_literals(&mc_vert);
    xsdk_built_shader_set_literals(built);
    const float to_clip[4] = { 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f };
    xsdk_gpu_set_vertex_constants(MC_VERT_PUSH, to_clip, MC_VERT_PUSH_REGISTERS);
    xsdk_gpu_set_pixel_constants((uint32_t)push, constants, (uint32_t)push_registers);
    for (int i = 0; i < count; i++)
        xsdk_gpu_set_texture((uint32_t)slots[i], textures[i], &point_clamp);
    xsdk_gpu_set_vertices(0, xsdk_gpu_address(quads) + (uint32_t)quad * QUAD_BYTES, QUAD_BYTES);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
    xsdk_gpu_resolve_texture(dst, 0);
}

/* Chroma plane p (1 Cb, 2 Cr): prediction plus residual into out[p]. */
static void chroma_pass(int p, const XsdkTexture *ref)
{
    float c[28] = { 0 };
    c[0] = (float)ref->width;
    c[1] = (float)ref->height;
    c[2] = 1.0f / (float)ref->width;
    c[3] = 1.0f / (float)ref->height;
    c[4] = 1.0f / (float)grid_texture.width;
    c[5] = 1.0f / (float)grid_texture.height;
    c[8] = 1.0f / (float)res_tex[p].width;
    c[9] = 1.0f / (float)res_tex[p].height;
    c[10] = 1.0f / (float)flags_tex.width;
    c[11] = 1.0f / (float)flags_tex.height;
    memcpy(&c[12], order, sizeof order);
    const int slots[4] = { MC_CHROMA_FRAG_SAMPLER_0_0, MC_CHROMA_FRAG_SAMPLER_0_1, MC_CHROMA_FRAG_SAMPLER_0_2,
                           MC_CHROMA_FRAG_SAMPLER_0_3 };
    const XsdkTexture *tex[4] = { ref, &grid_texture, &res_tex[p], &flags_tex };
    draw(&chroma_shader, &mc_chroma_frag, MC_CHROMA_FRAG_PUSH, MC_CHROMA_FRAG_PUSH_REGISTERS, c, 4, slots, tex, 1,
         (uint32_t)width / 8, (uint32_t)height / 2, &out[p]);
}

/* The multi-pass luma prediction into out[0]; with times, a fence after each pass times it. */
static bool luma_passes(const XsdkTexture *ref, GpuMcTimes *times)
{
    float c[28] = { 0 };
    uint64_t t = now_us();
    uint32_t *slot_times = times ? times->pass_us : NULL;
#define TIME_PASS(i) \
    if (slot_times) { \
        if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) \
            return false; \
        const uint64_t now = now_us(); \
        slot_times[i] = (uint32_t)(now - t); \
        t = now; \
    }
    /* G: the reference with margins, read as R8 (each sample clamped). */
    c[0] = (float)ref->width;
    c[1] = (float)ref->height;
    c[2] = 1.0f / (float)ref->width;
    c[3] = 1.0f / (float)ref->height;
    {
        const int slots[1] = { MC_PG_FRAG_SAMPLER_0_0 };
        const XsdkTexture *tex[1] = { ref };
        draw(&pg_shader, &mc_pg_frag, MC_PG_FRAG_PUSH, MC_PG_FRAG_PUSH_REGISTERS, c, 1, slots, tex, 3,
             (uint32_t)plane_w, (uint32_t)plane_h, &planes[PLANE_G]);
    }
    TIME_PASS(0)
    /* B, H and J from the G plane. */
    c[0] = 1.0f / (float)plane_w;
    c[1] = 1.0f / (float)plane_h;
    c[2] = c[3] = 0.0f;
    {
        const XsdkTexture *tex[1] = { &planes[PLANE_G] };
        int slots[1] = { MC_PB_FRAG_SAMPLER_0_0 };
        draw(&pb_shader, &mc_pb_frag, MC_PB_FRAG_PUSH, MC_PB_FRAG_PUSH_REGISTERS, c, 1, slots, tex, 3,
             (uint32_t)plane_w, (uint32_t)plane_h, &planes[PLANE_B]);
        TIME_PASS(1)
        slots[0] = MC_PH_FRAG_SAMPLER_0_0;
        draw(&ph_shader, &mc_ph_frag, MC_PH_FRAG_PUSH, MC_PH_FRAG_PUSH_REGISTERS, c, 1, slots, tex, 3,
             (uint32_t)plane_w, (uint32_t)plane_h, &planes[PLANE_H]);
        TIME_PASS(2)
        slots[0] = MC_PJ_FRAG_SAMPLER_0_0;
        draw(&pj_shader, &mc_pj_frag, MC_PJ_FRAG_PUSH, MC_PJ_FRAG_PUSH_REGISTERS, c, 1, slots, tex, 3,
             (uint32_t)plane_w, (uint32_t)plane_h, &planes[PLANE_J]);
        TIME_PASS(3)
    }
    /* The final pass: each block's quarter position from the planes. */
    c[0] = 1.0f / (float)plane_w;
    c[1] = 1.0f / (float)plane_h;
    c[4] = 1.0f / (float)grid_texture.width;
    c[5] = 1.0f / (float)grid_texture.height;
    c[8] = 1.0f / (float)res_tex[0].width;
    c[9] = 1.0f / (float)res_tex[0].height;
    c[10] = 1.0f / (float)flags_tex.width;
    c[11] = 1.0f / (float)flags_tex.height;
    memcpy(&c[12], order, sizeof order);
    {
        const int slots[7] = { MC_FINAL_FRAG_SAMPLER_0_0, MC_FINAL_FRAG_SAMPLER_0_1, MC_FINAL_FRAG_SAMPLER_0_2,
                               MC_FINAL_FRAG_SAMPLER_0_3, MC_FINAL_FRAG_SAMPLER_0_4, MC_FINAL_FRAG_SAMPLER_0_5,
                               MC_FINAL_FRAG_SAMPLER_0_6 };
        const XsdkTexture *tex[7] = { &planes[PLANE_G], &planes[PLANE_B], &planes[PLANE_H], &planes[PLANE_J],
                                      &grid_texture, &res_tex[0], &flags_tex };
        draw(&final_shader, &mc_final_frag, MC_FINAL_FRAG_PUSH, MC_FINAL_FRAG_PUSH_REGISTERS, c, 7, slots, tex, 0,
             (uint32_t)width / 4, (uint32_t)height, &out[0]);
    }
    TIME_PASS(4)
#undef TIME_PASS
    return true;
}

/* One pass: the quad over a target of w x h, the shader reading ref (if any) and the grid, resolved into dst. */
static void pass(const XsdkShader *pixel, const XsdkBuiltShader *built, int push, int push_registers, int ref_slot,
                 int grid_slot, const XsdkTexture *ref, int quad, uint32_t w, uint32_t h, const XsdkTexture *dst)
{
    xsdk_gpu_set_target(w, h);
    xsdk_gpu_set_shaders(&vertex_shader, pixel);
    xsdk_built_shader_set_literals(&mc_vert);
    xsdk_built_shader_set_literals(built);
    const float to_clip[4] = { 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f };
    xsdk_gpu_set_vertex_constants(MC_VERT_PUSH, to_clip, MC_VERT_PUSH_REGISTERS);
    float constants[24] = { 0 };
    if (ref) {
        constants[0] = (float)ref->width;
        constants[1] = (float)ref->height;
        constants[2] = 1.0f / (float)ref->width;
        constants[3] = 1.0f / (float)ref->height;
    }
    constants[4] = 1.0f / (float)grid_texture.width;
    constants[5] = 1.0f / (float)grid_texture.height;
    memcpy(&constants[8], order, sizeof order);
    xsdk_gpu_set_pixel_constants((uint32_t)push, constants, (uint32_t)push_registers);
    if (ref)
        xsdk_gpu_set_texture((uint32_t)ref_slot, ref, &point_clamp);
    xsdk_gpu_set_texture((uint32_t)grid_slot, &grid_texture, &point_clamp);
    xsdk_gpu_set_vertices(0, xsdk_gpu_address(quads) + (uint32_t)quad * QUAD_BYTES, QUAD_BYTES);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
    xsdk_gpu_resolve_texture(dst, 0);
}

bool gpumc_calibrate(char *report, size_t size)
{
    if (!ready)
        return false;
    /* Block (x, y) of rows 1-7 holds the vector (x - 16, 3y - 10). */
    int pitch, rows;
    uint32_t *grid = gpumc_grid(&pitch, &rows);
    for (int y = 1; y < PROBE_H; y++)
        for (int x = 0; x < PROBE_W; x++) {
            union { float f; uint32_t u; } v = { .f = (float)((x - 16 + 2048) * 4096 + (3 * y - 10 + 2048)) };
            grid[y * pitch + x] = v.u;
        }
    write_out(grid, (size_t)pitch * PROBE_H * 4);
    xsdk_gpu_invalidate(grid_texture.address, (uint32_t)pitch * PROBE_H * 4);
    pass(&probe_shader, &mc_probe_frag, MC_PROBE_FRAG_PUSH, MC_PROBE_FRAG_PUSH_REGISTERS, -1,
         MC_PROBE_FRAG_SAMPLER_0_1, NULL, 2, PROBE_W, PROBE_H, &probe_out);
    if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) {
        snprintf(report, size, "the calibration pass never finished");
        return false;
    }
    drop(probe_memory, probe_out.size_bytes);
    static uint8_t rows_out[PROBE_H][PROBE_W * 4];
    xsdk_texture_download_rows(&probe_out, 0, 0, PROBE_H, rows_out, PROBE_W * 4);

    /* Row 0: memory byte k holds channel rows_out[0][k] - 1; that channel must carry pixel k. */
    int channel_of_byte[4];
    bool seen[4] = { false, false, false, false };
    for (int k = 0; k < 4; k++) {
        channel_of_byte[k] = rows_out[0][k] - 1;
        if (channel_of_byte[k] < 0 || channel_of_byte[k] > 3 || seen[channel_of_byte[k]]) {
            snprintf(report, size, "byte order probe read %u %u %u %u, not a permutation of 1-4", rows_out[0][0],
                     rows_out[0][1], rows_out[0][2], rows_out[0][3]);
            return false;
        }
        seen[channel_of_byte[k]] = true;
    }
    memset(order, 0, sizeof order);
    for (int k = 0; k < 4; k++)
        order[k * 4 + channel_of_byte[k]] = 1.0f;   /* column k (pixel k) into row channel_of_byte[k] */

    /* Rows 1-7: the grid read back, red (+128) and green (+128) in whatever bytes they landed. */
    int byte_of_channel[4];
    for (int k = 0; k < 4; k++)
        byte_of_channel[channel_of_byte[k]] = k;
    int wrong = 0;
    for (int y = 1; y < PROBE_H; y++)
        for (int x = 0; x < PROBE_W; x++) {
            const int mvx = rows_out[y][x * 4 + byte_of_channel[0]] - 128;
            const int mvy = rows_out[y][x * 4 + byte_of_channel[1]] - 128;
            wrong += mvx != x - 16 || mvy != 3 * y - 10;
        }
    snprintf(report, size, "output bytes carry channels %d %d %d %d; grid read back with %d of %d blocks wrong",
             channel_of_byte[0], channel_of_byte[1], channel_of_byte[2], channel_of_byte[3], wrong,
             PROBE_W * (PROBE_H - 1));
    return wrong == 0;
}

/* What the CPU wrote while parsing, out of its cache for the GPU: the grid, the residuals, their flags. */
static void write_inputs(void)
{
    write_out(grid_memory, grid_texture.size_bytes);
    xsdk_gpu_invalidate(grid_texture.address, grid_texture.size_bytes);
    for (int p = 0; p < PLANES; p++) {
        write_out(res_memory[p], res_tex[p].size_bytes);
        xsdk_gpu_invalidate(res_tex[p].address, res_tex[p].size_bytes);
    }
    write_out(flags_memory, flags_tex.size_bytes);
    xsdk_gpu_invalidate(flags_tex.address, flags_tex.size_bytes);
}

static bool reference_texture(XsdkTexture *t, uint8_t *plane, int w, int h, int pitch)
{
    if (!NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_R8, (uint32_t)w, (uint32_t)h, 1, false)) ||
        t->pitch != (uint32_t)pitch || ((uintptr_t)plane & 4095))
        return false;
    xsdk_texture_attach(t, (volatile uint32_t *)plane);
    xsdk_gpu_invalidate(t->address, (uint32_t)pitch * (uint32_t)h);
    return true;
}

/* A texture of the SDK's own (write-combined memory), w x h, holding src's rows. */
static bool own_r8(XsdkTexture *t, bool *made, int w, int h, const uint8_t *src, int src_pitch)
{
    if (!*made && !NT_SUCCESS(xsdk_texture_create(t, XSDK_FORMAT_R8, (uint32_t)w, (uint32_t)h, 1, false)))
        return false;
    *made = true;
    xsdk_texture_upload(t, 0, src, (uint32_t)src_pitch);
    xsdk_gpu_invalidate(t->address, t->size_bytes);
    return true;
}

int gpumc_try(int what, uint8_t *const ref[3], const int pitch[3])
{
    if (!ready || hung)
        return -1;
    static XsdkTexture small, big;
    static bool small_made, big_made;
    static XsdkSamplerState linear_clamp;
    XsdkTexture tex;
    const XsdkSamplerState *sampler = &point_clamp;
    bool big_target = false;
    switch (what) {
    case GPUMC_TRY_GRID_AS_REFERENCE:
        tex = grid_texture;
        break;
    case GPUMC_TRY_SMALL_R8:
        if (!own_r8(&small, &small_made, 64, 64, ref[0], pitch[0]))
            return -1;
        tex = small;
        break;
    case GPUMC_TRY_BIG_R8_LINEAR:
    case GPUMC_TRY_BIG_R8_POINT: {
        if (!own_r8(&big, &big_made, width, height, ref[0], pitch[0]))
            return -1;
        tex = big;
        if (what == GPUMC_TRY_BIG_R8_LINEAR) {
            const XsdkSampler s = { .mag_filter = XSDK_FILTER_LINEAR, .min_filter = XSDK_FILTER_LINEAR,
                                    .mip_filter = XSDK_FILTER_POINT, .address_u = XSDK_ADDRESS_CLAMP,
                                    .address_v = XSDK_ADDRESS_CLAMP };
            xsdk_texture_sampler_state(&linear_clamp, &s);
            sampler = &linear_clamp;
        }
        break;
    }
    case GPUMC_TRY_FRAME_POINT:
        if (!reference_texture(&tex, ref[0], width, height, pitch[0]))
            return -1;
        break;
    default:   /* GPUMC_TRY_FRAME_BIG_TARGET */
        if (!reference_texture(&tex, ref[0], width, height, pitch[0]))
            return -1;
        big_target = true;
        break;
    }
    printf("[I] gpumc: try %d: texture %ux%u, format %u, pitch %u, at 0x%08X\n", what, (unsigned)tex.width,
           (unsigned)tex.height, (unsigned)tex.format, (unsigned)tex.pitch, (unsigned)tex.address);
    fflush(stdout);
    const XsdkSamplerState saved = point_clamp;
    point_clamp = *sampler;
    if (big_target)
        pass(&copy_shader, &mc_copy_frag, MC_COPY_FRAG_PUSH, MC_COPY_FRAG_PUSH_REGISTERS, MC_COPY_FRAG_SAMPLER_0_0,
             MC_COPY_FRAG_SAMPLER_0_1, &tex, 0, (uint32_t)width / 4, (uint32_t)height, &out[0]);
    else
        pass(&copy_shader, &mc_copy_frag, MC_COPY_FRAG_PUSH, MC_COPY_FRAG_PUSH_REGISTERS, MC_COPY_FRAG_SAMPLER_0_0,
             MC_COPY_FRAG_SAMPLER_0_1, &tex, 2, PROBE_W, PROBE_H, &probe_out);
    point_clamp = saved;
    if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) {
        hung = true;
        return 0;
    }
    return 1;
}

bool gpumc_test(int stage, uint8_t *const ref[3], uint8_t *const dst[3], const int pitch[3])
{
    if (!ready || hung)
        return false;
    write_inputs();
    XsdkTexture refs[PLANES];
    if (!reference_texture(&refs[0], ref[0], width, height, pitch[0]) ||
        !reference_texture(&refs[1], ref[1], width / 2, height / 2, pitch[1]) ||
        !reference_texture(&refs[2], ref[2], width / 2, height / 2, pitch[2]))
        return false;
    int plane;
    if (stage == GPUMC_TEST_COPY) {
        plane = 0;
        pass(&copy_shader, &mc_copy_frag, MC_COPY_FRAG_PUSH, MC_COPY_FRAG_PUSH_REGISTERS, MC_COPY_FRAG_SAMPLER_0_0,
             MC_COPY_FRAG_SAMPLER_0_1, &refs[0], 0, (uint32_t)width / 4, (uint32_t)height, &out[0]);
    } else if (stage == GPUMC_TEST_CHROMA) {
        plane = 1;
        chroma_pass(1, &refs[1]);
    } else {
        plane = 0;
        if (!luma_passes(&refs[0], NULL)) {
            hung = true;
            return false;
        }
    }
    if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) {
        hung = true;
        return false;
    }
    drop(out_memory[plane], out[plane].size_bytes);
    xsdk_texture_download_rows(&out[plane], 0, 0, out[plane].height, dst[plane], (uint32_t)pitch[plane]);
    return true;
}

/* A resolved 32-bit texture's rows into a plane, 4 bytes a texel. In the tiled layout each aligned run of 4 texels of
 * a row is 16 bytes together (src/tiled.c: x & 3 in the lowest bits; x & 4 goes elsewhere), and the layout repeats
 * every 32 rows a band further on, so a table of the first 32 rows' run offsets serves every row. The SDK's download
 * worked out every word's place and took 22 ms a 1080p frame. */
enum { BAND_ROWS = 32, MAX_RUNS = 1024 };
typedef struct {
    uint32_t pitch, runs, band_bytes;
    bool valid;
    uint32_t offset[BAND_ROWS][MAX_RUNS / 4];
} UntileTable;
static UntileTable untile_tables[PLANES];

static void build_untile_tables(void)
{
    for (int p = 0; p < PLANES; p++) {
        UntileTable *u = &untile_tables[p];
        u->pitch = out[p].pitch;
        u->runs = out[p].width / 4;
        u->valid = u->runs <= MAX_RUNS / 4;
        if (!u->valid)
            continue;
        for (uint32_t y = 0; y < BAND_ROWS; y++)
            for (uint32_t r = 0; r < u->runs; r++)
                u->offset[y][r] = xsdk_tiled_2d(r * 4, y, u->pitch, 2);
        u->band_bytes = xsdk_tiled_2d(0, BAND_ROWS, u->pitch, 2) - xsdk_tiled_2d(0, 0, u->pitch, 2);
        /* The table's premise, checked: a band further down is the same plus band_bytes, and runs are contiguous. */
        for (uint32_t y = BAND_ROWS; y < out[p].height && u->valid; y += 7)
            for (uint32_t r = 0; r < u->runs && u->valid; r += 13)
                for (uint32_t x = r * 4; x < r * 4 + 4 && u->valid; x++)
                    u->valid = xsdk_tiled_2d(x, y, u->pitch, 2) ==
                               u->offset[y % BAND_ROWS][r] + (y / BAND_ROWS) * u->band_bytes + (x - r * 4) * 4;
        if (!u->valid)
            printf("[W] gpumc: plane %d's tiling isn't as the un-tiler expects; it goes texel by texel\n", p);
    }
}

static void untile(int p, const uint8_t *src, uint8_t *dst, int dst_pitch)
{
    const XsdkTexture *t = &out[p];
    const UntileTable *u = &untile_tables[p];
    for (uint32_t y = 0; y < t->height; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)dst_pitch;
        if (u->valid) {
            const uint32_t *offsets = u->offset[y % BAND_ROWS];
            const uint8_t *band = src + (y / BAND_ROWS) * u->band_bytes;
            for (uint32_t r = 0; r < u->runs; r++)
                memcpy(row + r * 16, band + offsets[r], 16);
        } else {
            for (uint32_t x = 0; x < t->width; x++)
                memcpy(row + x * 4, src + xsdk_tiled_2d(x, y, t->pitch, 2), 4);
        }
    }
}

bool gpumc_predict(uint8_t *const ref[3], uint8_t *const dst[3], const int pitch[3], GpuMcTimes *times)
{
    if (!ready || hung)
        return false;
    uint64_t t0 = now_us();
    write_inputs();
    XsdkTexture refs[PLANES];
    if (!reference_texture(&refs[0], ref[0], width, height, pitch[0]) ||
        !reference_texture(&refs[1], ref[1], width / 2, height / 2, pitch[1]) ||
        !reference_texture(&refs[2], ref[2], width / 2, height / 2, pitch[2])) {
        static bool warned;
        if (!warned)
            printf("[E] gpumc: the reference planes don't fit linear textures (pitch %d)\n", pitch[0]);
        warned = true;
        return false;
    }
    uint64_t t1 = now_us();
    NTSTATUS st = STATUS_SUCCESS;
    if (!luma_passes(&refs[0], times && times->split ? times : NULL))
        st = STATUS_IO_TIMEOUT;
    uint64_t t_luma = now_us();
    for (int p = 1; p < PLANES; p++)
        chroma_pass(p, &refs[p]);
    if (NT_SUCCESS(st))
        st = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
    uint64_t t2 = now_us();
    if (!NT_SUCCESS(st)) {
        printf("[E] gpumc: the passes never finished: 0x%08X; the GPU is left alone from now on\n", (unsigned)st);
        hung = true;
        return false;
    }
    for (int p = 0; p < PLANES; p++) {
        drop(out_memory[p], out[p].size_bytes);
        if (times && times->sdk_download)
            xsdk_texture_download_rows(&out[p], 0, 0, out[p].height, dst[p], (uint32_t)pitch[p]);
        else
            untile(p, out_memory[p], dst[p], pitch[p]);
    }
    uint64_t t3 = now_us();
    if (times) {
        times->flush_us = (uint32_t)(t1 - t0);
        times->gpu_us = (uint32_t)(t2 - t1);
        times->luma_us = (uint32_t)(t_luma - t1);
        times->detile_us = (uint32_t)(t3 - t2);
    }
    return true;
}
