#include "render.h"

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

#include "moonlight360_shaders.h"
#include "video.h"
#include "xsdk/gpu.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

enum {
    SETS = 2,           /* texture sets in flight: one drawn while the other is written */
    PLANES = 3,
    VERTEX_FLOATS = 4,  /* x, y, u, v */
    CORNERS = 4,
    STRIDE_BYTES = VERTEX_FLOATS * 4,
    VERTICES_BYTES = CORNERS * STRIDE_BYTES,
    FENCE_TIMEOUT_MS = 500,
};

static const XsdkVertexElement layout[] = {
    { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 0 },
    { .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 8 },
};

typedef struct {
    XsdkTexture planes[PLANES];
    uint32_t fence;     /* GPU work that last read this set, 0 if none */
    bool created;       /* planes have memory of their own, that frames are copied into */
    AVFrame *frame;     /* a frame drawn in place (video_frame_in_gpu_memory): kept until the GPU is done with it */
} TextureSet;

static XsdkShader vertex_shader, pixel_shader;
static int frame_qp = -1;           /* the shown frame's quantiser */
static unsigned qp_frames, qp_sum;  /* for render_average_qp: frames with a quantiser since its last call, their sum */
static volatile uint32_t *vertices;
static XsdkSamplerState sampler;
static TextureSet sets[SETS];
static int current = -1;           /* set holding the shown frame */
static int drawn_this_frame = -1;  /* set the frame being built reads */
static uint32_t width, height;     /* stream size the sets are made for */
static float coefficients[12];     /* push constants: y_coef, u_coef, v_coef */
/* Tiled by default: with linear R8 planes, every stream that drew video eventually killed the console's network
 * until a reboot, and none that didn't draw them did (2026-11-22). Tiled is the layout 360 titles normally use. */
static bool use_tiled = true;

void render_set_tiled(bool tiled)
{
    use_tiled = tiled;
}

bool render_init(void)
{
    NTSTATUS st = xsdk_shader_load(&vertex_shader, yuv_vert.container, yuv_vert.container_bytes);
    if (NT_SUCCESS(st))
        st = xsdk_shader_load(&pixel_shader, yuv_frag.container, yuv_frag.container_bytes);

    if (NT_SUCCESS(st))
        st = xsdk_shader_set_vertex_layout(&vertex_shader, layout, 2, (const uint32_t[]){ STRIDE_BYTES });
    if (!NT_SUCCESS(st)) {
        printf("[E] render: loading shaders failed: 0x%08X\n", (unsigned)st);
        return false;
    }
    vertices = xsdk_gpu_alloc(VERTICES_BYTES);
    if (!vertices)
        return false;
    /* Triangle strip covering the screen; v = 0 is the texture's first row, at the top. */
    const float quad[CORNERS][VERTEX_FLOATS] = {
        { -1.0f, 1.0f, 0.0f, 0.0f },
        { 1.0f, 1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 1.0f },
        { 1.0f, -1.0f, 1.0f, 1.0f },
    };
    uint32_t words[VERTICES_BYTES / 4];
    memcpy(words, quad, sizeof quad);
    for (unsigned i = 0; i < VERTICES_BYTES / 4; i++)
        vertices[i] = words[i];
    xsdk_gpu_invalidate(xsdk_gpu_address(vertices), VERTICES_BYTES);

    const XsdkSampler s = {
        .mag_filter = XSDK_FILTER_LINEAR,
        .min_filter = XSDK_FILTER_LINEAR,
        .mip_filter = XSDK_FILTER_POINT,
        .address_u = XSDK_ADDRESS_CLAMP,
        .address_v = XSDK_ADDRESS_CLAMP,
    };
    xsdk_texture_sampler_state(&sampler, &s);
    return true;
}

static void free_sets(void)
{
    for (int i = 0; i < SETS; i++) {
        if (sets[i].fence)
            xsdk_gpu_wait(sets[i].fence, FENCE_TIMEOUT_MS);
        if (sets[i].created)
            for (int p = 0; p < PLANES; p++)
                xsdk_texture_free(&sets[i].planes[p]);
        av_frame_free(&sets[i].frame);
        memset(&sets[i], 0, sizeof sets[i]);
    }
    current = -1;
    width = height = 0;
}

void render_shutdown(void)
{
    free_sets();
    if (vertices)
        xsdk_gpu_free(vertices);
    vertices = NULL;
    xsdk_shader_free(&pixel_shader);
    xsdk_shader_free(&vertex_shader);
}

int render_frame_qp(void)
{
    return frame_qp;
}

int render_average_qp(void)
{
    const int qp = qp_frames ? (int)((qp_sum + qp_frames / 2) / qp_frames) : -1;
    qp_frames = qp_sum = 0;
    return qp;
}

void render_clear(void)
{
    free_sets();
}

static bool create_sets(uint32_t w, uint32_t h)
{
    free_sets();
    for (int i = 0; i < SETS; i++) {
        for (int p = 0; p < PLANES; p++) {
            uint32_t pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h;
            NTSTATUS st = xsdk_texture_create(&sets[i].planes[p], XSDK_FORMAT_R8, pw, ph, 1, use_tiled);
            if (!NT_SUCCESS(st)) {
                printf("[E] render: texture %ux%u failed: 0x%08X\n", (unsigned)pw, (unsigned)ph, (unsigned)st);
                sets[i].created = p > 0;
                free_sets();
                return false;
            }
        }
        sets[i].created = true;
    }
    width = w;
    height = h;
    printf("[I] render: %s textures for %ux%u\n", use_tiled ? "tiled" : "linear", (unsigned)w, (unsigned)h);
    return true;
}

/* BT.601 or BT.709, limited or full range, as the frame says (Sunshine marks its streams). */
static void set_coefficients(const AVFrame *f)
{
    bool bt709 = f->colorspace == AVCOL_SPC_BT709;
    bool full = f->color_range == AVCOL_RANGE_JPEG;
    float kr = bt709 ? 0.2126f : 0.299f, kb = bt709 ? 0.0722f : 0.114f, kg = 1.0f - kr - kb;
    float y_scale = full ? 1.0f : 255.0f / 219.0f, y_offset = full ? 0.0f : 16.0f / 255.0f;
    float c_scale = full ? 1.0f : 255.0f / 224.0f;
    float u_g = -2.0f * kb * (1.0f - kb) / kg, v_g = -2.0f * kr * (1.0f - kr) / kg;
    /* The frame's quantiser (H.264's QP; video.c puts it in the quality field), for the HUD and the log. */
    frame_qp = f->quality >= 0 && f->quality <= 51 ? f->quality : -1;
    if (frame_qp >= 0) {
        qp_frames++;
        qp_sum += (unsigned)frame_qp;
    }
    const float c[12] = {
        y_scale, y_offset, 0, 0,
        0.0f, u_g * c_scale, 2.0f * (1.0f - kb) * c_scale, 0,
        2.0f * (1.0f - kr) * c_scale, v_g * c_scale, 0.0f, 0,
    };
    memcpy(coefficients, c, sizeof c);
}

/* A linear R8 texture of w by h texels whose rows are pitch bytes apart. xsdk gives a texture the least pitch its
 * width allows; video.c may lay rows further apart than that, and such a pitch is the least one of a texture as wide
 * as the pitch itself, whose first fetch word (type, pitch, tiling) and sizes in memory this texture takes over. */
static bool plane_texture(XsdkTexture *t, uint32_t w, uint32_t h, uint32_t pitch)
{
    XsdkTexture wide;
    if (!NT_SUCCESS(xsdk_texture_init(t, XSDK_FORMAT_R8, w, h, 1, false)))
        return false;
    if (t->pitch == pitch)
        return true;
    if (pitch < t->pitch || !NT_SUCCESS(xsdk_texture_init(&wide, XSDK_FORMAT_R8, pitch, h, 1, false)) ||
        wide.pitch != pitch)
        return false;
    t->pitch = wide.pitch;
    t->size_bytes = wide.size_bytes;
    t->mip_offset_bytes = wide.mip_offset_bytes;
    t->fetch[0] = wide.fetch[0];
    return true;
}

/* Textures over a frame's own planes, as video.c laid them out; false if they don't line up. */
static bool attach_frame(TextureSet *set, const AVFrame *f)
{
    for (int p = 0; p < PLANES; p++) {
        uint32_t pw = p ? (uint32_t)(f->width + 1) / 2 : (uint32_t)f->width;
        uint32_t ph = p ? (uint32_t)(f->height + 1) / 2 : (uint32_t)f->height;
        XsdkTexture *t = &set->planes[p];
        if (f->linesize[p] <= 0 || !plane_texture(t, pw, ph, (uint32_t)f->linesize[p]) || ((uintptr_t)f->data[p] & 4095))
            return false;
        xsdk_texture_attach(t, (volatile uint32_t *)f->data[p]);
        /* The decoder wrote the planes out of the CPU's cache (video.c); the GPU's caches may hold an older frame. */
        xsdk_gpu_invalidate(t->address, (uint32_t)f->linesize[p] * ph);
    }
    return true;
}

void render_set_frame(AVFrame *f)
{
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        static int warned;
        if (!warned++)
            printf("[E] render: unsupported pixel format %s\n", av_get_pix_fmt_name(f->format));
        av_frame_free(&f);
        return;
    }
    int next = (current + 1) % SETS;
    TextureSet *set = &sets[next];
    if (set->fence) {
        NTSTATUS st = xsdk_gpu_wait(set->fence, FENCE_TIMEOUT_MS);
        if (!NT_SUCCESS(st))
            printf("[W] render: fence wait failed: 0x%08X\n", (unsigned)st);
        set->fence = 0;
    }
    av_frame_free(&set->frame);   /* the GPU is done with the frame this set showed last */

    if (video_frame_in_gpu_memory(f) && !set->created && attach_frame(set, f)) {
        static bool logged;
        if (!logged)
            printf("[I] render: drawing frames in place, %dx%d\n", f->width, f->height);
        logged = true;
        set->frame = f;
        set_coefficients(f);
        current = next;
        return;
    }

    /* Copied into textures of the set's own (a frame from elsewhere, or planes that don't line up). */
    if ((uint32_t)f->width != width || (uint32_t)f->height != height || !set->created) {
        if (!create_sets(f->width, f->height)) {
            av_frame_free(&f);
            return;
        }
        set = &sets[next];
    }
    for (int p = 0; p < PLANES; p++) {
        xsdk_texture_upload(&set->planes[p], 0, f->data[p], (uint32_t)f->linesize[p]);
        xsdk_gpu_invalidate(set->planes[p].address, set->planes[p].size_bytes);
    }
    set_coefficients(f);
    current = next;
    av_frame_free(&f);
}

bool render_draw(uint32_t target_width, uint32_t target_height)
{
    drawn_this_frame = -1;
    if (current < 0)
        return false;
    TextureSet *set = &sets[current];
    xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
    xsdk_built_shader_set_literals(&yuv_vert);
    xsdk_built_shader_set_literals(&yuv_frag);
    xsdk_gpu_set_pixel_constants(YUV_FRAG_PUSH, coefficients, YUV_FRAG_PUSH_REGISTERS);
    xsdk_gpu_set_texture(YUV_FRAG_SAMPLER_0_0, &set->planes[0], &sampler);
    xsdk_gpu_set_texture(YUV_FRAG_SAMPLER_0_1, &set->planes[1], &sampler);
    xsdk_gpu_set_texture(YUV_FRAG_SAMPLER_0_2, &set->planes[2], &sampler);
    xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices), VERTICES_BYTES);
    xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_STRIP, CORNERS);
    drawn_this_frame = current;
    (void)target_width, (void)target_height;
    return true;
}

void render_frame_submitted(void)
{
    if (drawn_this_frame >= 0)
        sets[drawn_this_frame].fence = xsdk_gpu_fence();
}
