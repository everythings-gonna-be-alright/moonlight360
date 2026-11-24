/*
 * Draws the app's menus on a PC, into PNG files, from the same menu.c, ui.c, overlay.c and gfx.c the console runs.
 * The GPU calls are stood in for by a software rasteriser that does what shape.frag and xsdk's text do: rounded
 * rectangles by signed distance, blended by alpha, and the 10x20 bitmap font point-sampled.
 *
 *     dev/menu_preview/build.sh      (writes the PNGs to build/preview)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "apps.h"
#include "config.h"
#include "gfx.h"
#include "menu.h"
#include "moon.h"
#include "overlay.h"
#include "uisound.h"
#include "discover.h"
#include "image.h"
#include "coverart.h"
#include "logbuf.h"
#include "xsdk/texture.h"
#include "perf.h"
#include "ui.h"

#include "../../third_party/xsdk/src/text_font.h"

enum { W = 1280, H = 720, RING_BASE = 0x10000000 };

static float fb[H][W][3];

static void blend_pixel(int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f)
        return;
    if (a > 1.0f)
        a = 1.0f;
    float *p = fb[y][x];
    p[0] = r * a + p[0] * (1.0f - a);
    p[1] = g * a + p[1] * (1.0f - a);
    p[2] = b * a + p[2] * (1.0f - a);
}

/* ---- GPU stand-ins ---- */

static volatile uint32_t *ring;
static uint32_t vertices_address;

volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes)
{
    ring = calloc(1, size_bytes);
    return ring;
}
void xsdk_gpu_free(volatile uint32_t *memory) { free((void *)memory); }
uint32_t xsdk_gpu_address(volatile const uint32_t *memory) { (void)memory; return RING_BASE; }
uint32_t xsdk_gpu_fence(void) { return 1; }
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms) { (void)fence, (void)timeout_ms; return 0; }
void xsdk_gpu_invalidate(uint32_t address, uint32_t size_bytes) { (void)address, (void)size_bytes; }
void xsdk_gpu_blend_state(XsdkBlendState *state, const XsdkBlend *setting) { (void)state, (void)setting; }
void xsdk_gpu_depth_stencil_state(XsdkDepthStencilState *s, const XsdkDepthStencil *d) { (void)s, (void)d; }
void xsdk_gpu_raster_state(XsdkRasterState *s, const XsdkRaster *r) { (void)s, (void)r; }
void xsdk_gpu_set_blend(const XsdkBlendState *state) { (void)state; }
void xsdk_gpu_set_depth_stencil(const XsdkDepthStencilState *state) { (void)state; }
void xsdk_gpu_set_raster(const XsdkRasterState *state) { (void)state; }
void xsdk_gpu_set_vertex_constants(uint32_t first, const float *values, uint32_t count) { (void)first, (void)values, (void)count; }
void xsdk_gpu_set_vertices(uint32_t stream, uint32_t address, uint32_t size_bytes)
{
    (void)stream, (void)size_bytes;
    vertices_address = address;
}
NTSTATUS xsdk_shader_load(XsdkShader *shader, const void *container, uint32_t bytes) { (void)shader, (void)container, (void)bytes; return 0; }
void xsdk_shader_free(XsdkShader *shader) { (void)shader; }
/* A shader keeps its vertex stride, so a draw knows whether it's shapes (48 bytes a corner) or glyphs (32). */
NTSTATUS xsdk_shader_set_vertex_layout(XsdkShader *s, const XsdkVertexElement *e, uint32_t c, const uint32_t *st) { (void)e, (void)c; s->x = st[0]; return 0; }
static const XsdkShader *current_vertex_shader;
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel) { (void)pixel; current_vertex_shader = vertex; }

static const XsdkTexture *current_texture;
NTSTATUS xsdk_texture_create(XsdkTexture *t, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, bool tiled)
{
    (void)levels, (void)tiled;
    t->width = width;
    t->height = height;
    t->format = format;
    t->texels = calloc(width * (format == XSDK_FORMAT_R8G8B8A8 ? 4 : 1), height);
    return t->texels ? 0 : -1;
}
void xsdk_texture_upload(XsdkTexture *t, uint32_t level, const void *data, uint32_t row_bytes)
{
    (void)level;
    const uint32_t bpp = t->format == XSDK_FORMAT_R8G8B8A8 ? 4 : 1;
    for (uint32_t row = 0; row < t->height; row++)
        memcpy(t->texels + row * t->width * bpp, (const uint8_t *)data + row * row_bytes, t->width * bpp);
}
void xsdk_texture_free(XsdkTexture *t) { free(t->texels); t->texels = NULL; }
void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler) { (void)state, (void)sampler; }
void xsdk_gpu_set_texture(uint32_t slot, const XsdkTexture *t, const XsdkSamplerState *s) { (void)slot, (void)s; current_texture = t; }
void xsdk_built_shader_set_literals(const XsdkBuiltShader *built) { (void)built; }

static float word_float(uint32_t w)
{
    union { uint32_t u; float f; } c = { .u = w };
    return c.f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A shape's four corners as gfx.c writes them: top left, top right, bottom left, bottom right; shape.frag per pixel. */
static void draw_shape(const volatile uint32_t *v)
{
    float c[4][12];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 12; k++)
            c[i][k] = word_float(v[i * 12 + k]);
    const float x0 = c[0][0], y0 = c[0][1], x1 = c[3][0], y1 = c[3][1];
    const float hw = c[0][8], hh = c[0][9], radius = c[0][10], inv = c[0][11];
    for (int y = (int)ceilf(y0); y < y1; y++) {
        const float ty = (y - y0) / (y1 - y0);
        const float ly = c[0][7] + ty * (c[3][7] - c[0][7]);
        float col[4];
        for (int k = 0; k < 4; k++)
            col[k] = c[0][2 + k] + ty * (c[3][2 + k] - c[0][2 + k]);
        for (int x = (int)ceilf(x0); x < x1; x++) {
            const float tx = (x - x0) / (x1 - x0);
            const float lx = c[0][6] + tx * (c[3][6] - c[0][6]);
            const float qx = fabsf(lx) - hw + radius, qy = fabsf(ly) - hh + radius;
            const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            const float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - radius;
            const float coverage = clampf(0.5f - d * inv, 0.0f, 1.0f);
            blend_pixel(x, y, col[0], col[1], col[2], col[3] * coverage);
        }
    }
}

void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count)
{
    (void)primitive_type, (void)vertex_count;
    draw_shape(ring + (vertices_address - RING_BASE) / 4);
}

/* A glyph: four corners of position, colour and atlas coordinate (8 words each); the atlas sampled at each pixel's
 * centre, as the GPU does it 1:1. */
static void draw_glyph(const volatile uint32_t *v)
{
    float c[4][8];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            c[i][k] = word_float(v[i * 8 + k]);
    const float x0 = c[0][0], y0 = c[0][1], x1 = c[3][0], y1 = c[3][1];
    const XsdkTexture *t = current_texture;
    for (int y = (int)ceilf(y0); y < y1; y++) {
        const float vv = c[0][7] + (y - y0) / (y1 - y0) * (c[3][7] - c[0][7]);
        const int ty = (int)(vv * t->height);
        for (int x = (int)ceilf(x0); x < x1; x++) {
            const float uu = c[0][6] + (x - x0) / (x1 - x0) * (c[3][6] - c[0][6]);
            const int tx = (int)(uu * t->width);
            if (tx < 0 || ty < 0 || tx >= (int)t->width || ty >= (int)t->height)
                continue;
            if (t->format == XSDK_FORMAT_R8G8B8A8) {   /* an image: its colour times the tint */
                const uint8_t *px = &t->texels[(ty * t->width + tx) * 4];
                blend_pixel(x, y, c[0][2] * px[0] / 255.0f, c[0][3] * px[1] / 255.0f, c[0][4] * px[2] / 255.0f,
                            c[0][5] * px[3] / 255.0f);
            } else {
                blend_pixel(x, y, c[0][2], c[0][3], c[0][4], c[0][5] * t->texels[ty * t->width + tx] / 255.0f);
            }
        }
    }
}

/* gfx.c's batches: six indices, two triangles, a quad of four corners each: shapes (48 words) or glyphs (32). */
void xsdk_gpu_draw_indexed(uint32_t primitive_type, uint32_t index_address, uint32_t index_count, bool indices_32_bit)
{
    (void)primitive_type, (void)index_address, (void)indices_32_bit;
    const bool glyphs = current_vertex_shader && current_vertex_shader->x == 32;
    for (uint32_t q = 0; q < index_count / 6; q++) {
        const volatile uint32_t *v = ring + (vertices_address - RING_BASE) / 4;
        if (glyphs)
            draw_glyph(v + q * 32);
        else
            draw_shape(v + q * 48);
    }
}

/* ---- text stand-ins ---- */

static uint32_t next_point(const unsigned char **s)
{
    const unsigned char *p = *s;
    if (p[0] < 0x80) {
        *s = p + 1;
        return p[0];
    }
    int len = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : 2;
    uint32_t point = p[0] & (0x7F >> len);
    for (int i = 1; i < len && p[i]; i++)
        point = point << 6 | (p[i] & 0x3F);
    *s = p + len;
    return point;
}

static int glyph_of(uint32_t point)
{
    for (int i = 0; i < TEXT_FONT_GLYPHS; i++)
        if (text_font_code_points[i] == point)
            return i;
    fprintf(stderr, "preview: no glyph for U+%04X\n", (unsigned)point);
    return 0;
}

void xsdk_text_draw(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8)
{
    const float a = ((argb >> 24) & 0xFF) / 255.0f, r = ((argb >> 16) & 0xFF) / 255.0f,
                g = ((argb >> 8) & 0xFF) / 255.0f, b = (argb & 0xFF) / 255.0f;
    int32_t pen = x, top = y;
    for (const unsigned char *s = (const unsigned char *)utf8; *s;) {
        uint32_t point = next_point(&s);
        if (point == '\n') {
            pen = x;
            top += 20 * (int32_t)scale;
            continue;
        }
        if (point < 0x20)
            continue;
        const int glyph = glyph_of(point);
        for (int row = 0; row < 20; row++)
            for (int col = 0; col < 10; col++)
                if (text_font_rows[glyph][row] & (0x8000 >> col))
                    for (uint32_t sy = 0; sy < scale; sy++)
                        for (uint32_t sx = 0; sx < scale; sx++)
                            blend_pixel(pen + col * (int)scale + (int)sx, top + row * (int)scale + (int)sy, r, g, b, a);
        pen += 10 * (int32_t)scale;
    }
}

void xsdk_text_measure(const char *utf8, uint32_t scale, uint32_t *width, uint32_t *height)
{
    uint32_t longest = 0, line = 0, lines = 1;
    for (const unsigned char *s = (const unsigned char *)utf8; *s;) {
        uint32_t point = next_point(&s);
        if (point == '\n') {
            lines++;
            line = 0;
        } else if (point >= 0x20 && ++line > longest)
            longest = line;
    }
    *width = longest * 10 * scale;
    *height = lines * 20 * scale;
}

void uisound_play(UiSound sound) { (void)sound; }
/* Two PCs found, the search two thirds through. */
void discover_start(void) {}
bool discover_running(void) { return true; }
void discover_progress(int *tried, int *total) { *tried = 170; *total = 254; }
int discover_count(void) { return 2; }
void discover_get(int i, char *address, size_t address_size, char *name, size_t name_size)
{
    snprintf(address, address_size, "%s", i == 0 ? "192.168.1.100" : "192.168.1.57");
    snprintf(name, name_size, "%s", i == 0 ? "GAMING-PC" : "living-room");
}
void discover_stop(void) {}
size_t logbuf_recent(char *out, size_t size)
{
    return (size_t)snprintf(out, size, "%s",
        "2026-11-24 01:41:07.137 [I] Moonlight 360 starting (build Nov 24 2026 01:30:00)\n"
        "2026-11-24 01:41:14.274 display: 1280x720 scaled to the 1920x1080 output\n"
        "2026-11-24 01:41:21.411 [I] perf: temperatures CPU 74.8, GPU 71.5, eDRAM 66.8, board 44.1 C\n"
        "2026-11-24 01:41:28.548 [I] apps: covers: 7 kept, 0 fetched, 0 missing\n"
        "2026-11-24 01:41:35.685 [I] host 192.168.1.35, app \"Desktop\", 1920x1080 60 fps 20000 kbps\n"
        "2026-11-24 01:41:42.822 [I] session: Contacting 192.168.1.35...\n"
        "2026-11-24 01:41:49.959 [I] session: Starting \"Desktop\" at 1920x1080 60 fps, 20000 kbps...\n"
        "2026-11-24 01:41:56.096 [I] common-c: Starting RTSP handshake...done\n"
        "2026-11-24 01:41:03.233 [I] video: H.264 1920x1080@60, 6 decode threads at priority 0, 24 slices asked, deblocking on\n"
        "2026-11-24 01:42:10.370 [I] session: Streaming\n"
        "2026-11-24 01:42:17.507 [I] stats: decode 60 fps (11.8 ms)  shown 60 fps  dropped 1  errors 0  audio gaps 0  CAVLC lean  ping 1 ms\n"
        "2026-11-24 01:42:24.644 [W] video: decode unit queue overflow: 3 frames dropped\n"
        "2026-11-24 01:42:31.781 [I] stats: latency 37 ms = PC 5 + net 4 + decode 12 + display 16\n"
        "2026-11-24 01:42:38.918 [E] session: stage RTSP handshake failed: -1\n"
        "2026-11-24 01:42:45.055 [I] session: Disconnected.\n");
}

/* ---- keyboard, config and app list stand-ins ---- */

uint32_t xsdk_xam_keyboard(uint32_t user, const uint16_t *t, const uint16_t *d, const uint16_t *def, uint16_t *text,
                           uint32_t chars, XAM_OVERLAPPED *o)
{
    (void)user, (void)t, (void)d, (void)def, (void)text, (void)chars, (void)o;
    return 1;
}
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *o, uint32_t timeout_ms, uint32_t *result) { (void)o, (void)timeout_ms, (void)result; return ERROR_SUCCESS; }
bool config_save(const char *path, const Config *config) { (void)path, (void)config; return true; }
const char *menu_combo_name(int combo)
{
    static const char *const names[] = { "Back", "Start", "Back + Start", "LB + RB" };
    return names[combo >= 0 && combo < 4 ? combo : 0];
}
void config_remove_recent(Config *c, int i)
{
    for (int k = i; k < CONFIG_RECENT - 1; k++) {
        c->recent_id[k] = c->recent_id[k + 1];
        memcpy(c->recent_name[k], c->recent_name[k + 1], sizeof c->recent_name[k]);
    }
    c->recent_name[CONFIG_RECENT - 1][0] = 0;
}

/* The PC's apps: Sunshine's defaults and some games, with their covers (dev/menu_preview/covers); the last has
 * none, for the initial in its place. */
static const char *const fake_apps[] = { "Desktop", "Steam Big Picture", "Red Dead Redemption 2", "Grand Theft Auto VI",
                                         "Forza Horizon 6", "Cyberpunk 2077", "Retro Arcade" };
static const char *const fake_covers[] = { "desktop.png", "steam.png", "rdr2.jpg", "gta6.png", "forza6.jpg",
                                           "cyberpunk.jpg", NULL };
static AppsState fake_state = APPS_READY;
void apps_fetch(const char *host) { (void)host; }
AppsState apps_state(void) { return fake_state; }
void apps_message(char *out, size_t size) { snprintf(out, size, "Could not reach 192.168.1.100: Timeout was reached (-5)"); }
int apps_count(void) { return (int)(sizeof fake_apps / sizeof fake_apps[0]); }
void apps_get(int i, char *name, size_t size, bool *running)
{
    snprintf(name, size, "%s", fake_apps[i]);
    *running = i == 2;
}
unsigned apps_generation(void) { return 1; }
int apps_id(int i) { return i + 1; }
/* The Play tab's covers: the stand-in covers by id (app i + 1). */
const uint8_t *apps_cover(int i);
const uint8_t *coverart_get(const char *host, int id) { (void)host; return apps_cover(id - 1); }
unsigned coverart_generation(void) { return 1; }
/* The covers, read from dev/menu_preview/covers and fitted as apps.c fits the host's. */
const uint8_t *apps_cover(int i)
{
    static uint8_t *covers[8];
    static bool tried[8];
    if (i < 0 || i >= apps_count() || !fake_covers[i])
        return NULL;
    if (!tried[i]) {
        tried[i] = true;
        char path[128];
        snprintf(path, sizeof path, "dev/menu_preview/covers/%s", fake_covers[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            static uint8_t data[1 << 20];
            const size_t n = fread(data, 1, sizeof data, f);
            fclose(f);
            covers[i] = image_decode_fit(data, n, APPS_COVER_W, APPS_COVER_H);
        }
    }
    return covers[i];
}

/* Sample readings, as the console might show mid-stream. */
void perf_get(PerfReadings *out)
{
    const PerfReadings r = { .cpu = { 38, 22, 91, 84, 88, 79 }, .cpu_average = 67, .gpu = 18,
                             .temp_cpu = 63.4f, .temp_gpu = 58.9f, .temp_edram = 60.2f, .temp_board = 44.0f };
    *out = r;
}

/* ---- scenes ---- */

static void write_png(const char *path)
{
    gfx_flush();
    const size_t row = 1 + W * 3;
    unsigned char *raw = malloc(row * H);
    for (int y = 0; y < H; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < W; x++)
            for (int k = 0; k < 3; k++)
                raw[y * row + 1 + x * 3 + k] = (unsigned char)(clampf(fb[y][x][k], 0, 1) * 255.0f + 0.5f);
    }
    uLongf zlen = compressBound(row * H);
    unsigned char *z = malloc(zlen);
    compress2(z, &zlen, raw, row * H, 6);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13] = { W >> 24, (W >> 16) & 255, (W >> 8) & 255, W & 255, H >> 24, (H >> 16) & 255,
                               (H >> 8) & 255, H & 255, 8, 2, 0, 0, 0 };
    const struct { const char *type; const unsigned char *data; uLong len; } chunks[3] = {
        { "IHDR", ihdr, 13 }, { "IDAT", z, zlen }, { "IEND", NULL, 0 } };
    for (int i = 0; i < 3; i++) {
        unsigned char len[4] = { chunks[i].len >> 24, (chunks[i].len >> 16) & 255, (chunks[i].len >> 8) & 255,
                                 chunks[i].len & 255 };
        fwrite(len, 1, 4, f);
        fwrite(chunks[i].type, 1, 4, f);
        if (chunks[i].len)
            fwrite(chunks[i].data, 1, chunks[i].len, f);
        uLong crc = crc32(0, (const Bytef *)chunks[i].type, 4);
        if (chunks[i].len)
            crc = crc32(crc, chunks[i].data, chunks[i].len);
        unsigned char c[4] = { crc >> 24, (crc >> 16) & 255, (crc >> 8) & 255, crc & 255 };
        fwrite(c, 1, 4, f);
    }
    fclose(f);
    free(raw);
    free(z);
    printf("wrote %s\n", path);
}

static void clear(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
        {
            fb[y][x][0] = 0x0E / 255.0f;   /* the resolve's clear colour, main.c CLEAR_ARGB */
            fb[y][x][1] = 0x0E / 255.0f;
            fb[y][x][2] = 0x10 / 255.0f;
        }
}

static void press(uint16_t buttons)
{
    InputState in;
    memset(&in, 0, sizeof in);
    in.connected[0] = true;
    in.pressed[0] = buttons;
    gfx_frame_begin(W, H);
    menu_update(&in);
    memset(&in, 0, sizeof in);   /* released */
    gfx_frame_begin(W, H);
    menu_update(&in);
}

static void shot_menu(const char *dir, const char *name)
{
    char path[512];
    clear();
    gfx_frame_begin(W, H);
    menu_draw(W, H);
    snprintf(path, sizeof path, "%s/%s.png", dir, name);
    write_png(path);
}

/* A stand-in for a game frame under the stream menu. */
/* A stand-in for the game behind the stats HUD: a night sky over ranges of hills, each range nearer and darker. */
static float ridge(float x, int layer)
{
    const float f = 0.0021f * (1.0f + 0.55f * (float)layer), phase = 1.7f * (float)layer;
    return 0.50f * sinf(x * f + phase) + 0.30f * sinf(x * f * 2.3f + phase * 2.1f) +
           0.14f * sinf(x * f * 5.1f + phase * 0.7f) + 0.06f * sinf(x * f * 11.0f + phase * 3.3f);
}

static float smooth(float edge0, float edge1, float v)
{
    const float t = v <= edge0 ? 0.0f : v >= edge1 ? 1.0f : (v - edge0) / (edge1 - edge0);
    return t * t * (3.0f - 2.0f * t);
}

static void fake_game(void)
{
    static const float zenith[3] = { 0.020f, 0.027f, 0.066f }, horizon[3] = { 0.150f, 0.135f, 0.215f };
    static const float glow[3] = { 0.62f, 0.33f, 0.16f }, moon[3] = { 0.96f, 0.93f, 0.84f };
    static const float haze[3] = { 0.105f, 0.105f, 0.175f }, near[3] = { 0.012f, 0.014f, 0.026f };
    enum { LAYERS = 4, HORIZON_Y = 470, MOON_X = 968, MOON_Y = 178, MOON_R = 46 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float c[3];
            /* The sky: dark overhead, lighter toward the horizon, with a low warm glow where the day went down. */
            const float down = smooth(0.0f, (float)HORIZON_Y, (float)y);
            const float gx = ((float)x - 430.0f) / 520.0f, gy = ((float)y - (float)HORIZON_Y) / 150.0f;
            const float warm = 0.55f * expf(-(gx * gx + gy * gy));
            for (int k = 0; k < 3; k++)
                c[k] = zenith[k] + (horizon[k] - zenith[k]) * down * down + glow[k] * warm;
            /* Stars, fewer and fainter toward the horizon: one in some cells of a grid, placed by a hash. */
            const int cell_x = x / 26, cell_y = y / 26;
            uint32_t hash = (uint32_t)cell_x * 374761393u + (uint32_t)cell_y * 668265263u;
            hash = (hash ^ (hash >> 13)) * 1274126177u;
            hash ^= hash >> 16;
            if ((hash & 7) < 3) {
                const float sx = (float)(cell_x * 26) + 3.0f + (float)((hash >> 8) % 20);
                const float sy = (float)(cell_y * 26) + 3.0f + (float)((hash >> 14) % 20);
                const float d2 = ((float)x - sx) * ((float)x - sx) + ((float)y - sy) * ((float)y - sy);
                const float bright = (0.25f + 0.75f * (float)((hash >> 20) % 100) / 100.0f) * (1.0f - down);
                const float star = bright * expf(-d2 / (0.45f + 0.5f * (float)((hash >> 4) & 1)));
                for (int k = 0; k < 3; k++)
                    c[k] += star * (k == 2 ? 1.0f : 0.92f);
            }
            /* The moon and its halo. */
            const float md = sqrtf(((float)x - MOON_X) * ((float)x - MOON_X) + ((float)y - MOON_Y) * ((float)y - MOON_Y));
            const float disc = 1.0f - smooth((float)MOON_R - 1.0f, (float)MOON_R + 1.0f, md);
            const float halo = 0.16f * expf(-md / 95.0f) + 0.05f * expf(-md / 320.0f);
            for (int k = 0; k < 3; k++)
                c[k] = c[k] * (1.0f - disc) + moon[k] * disc + moon[k] * halo * (1.0f - disc);
            /* The hills, far to near: each range a little lower on the screen, taller and darker than the last. */
            for (int layer = 0; layer < LAYERS; layer++) {
                const float t = (float)layer / (float)(LAYERS - 1);
                const float top = (float)HORIZON_Y - 26.0f + 62.0f * (float)layer -
                                  (34.0f + 30.0f * t) * ridge((float)x + 400.0f * (float)layer, layer);
                const float cover = smooth(top - 1.0f, top + 1.0f, (float)y);
                const float depth = smooth(top, top + 220.0f, (float)y);   /* mist thins down the slope */
                for (int k = 0; k < 3; k++) {
                    const float crest = haze[k] + (near[k] - haze[k]) * t + glow[k] * warm * 0.22f * (1.0f - t);
                    const float hill = crest + (near[k] - crest) * depth * (0.45f + 0.55f * t);
                    c[k] = c[k] * (1.0f - cover) + hill * cover;
                }
            }
            /* Darker toward the corners. */
            const float vx = ((float)x - W / 2.0f) / (W / 2.0f), vy = ((float)y - H / 2.0f) / (H / 2.0f);
            const float vignette = 1.0f - 0.22f * (vx * vx + vy * vy);
            for (int k = 0; k < 3; k++) {
                const float v = c[k] * vignette;
                fb[y][x][k] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
            }
        }
}

/* ---- the README's tour (menu_preview <dir> tour) ---- */


/* Every how many of the menus' 60 frames a second one is written: 1, all of them (60 a second); 2, 30 a second. */
static int tour_stride = 1;

typedef struct {
    uint16_t button;   /* pressed for one frame */
    int wait;          /* then this many frames of the menu as it is (60 a second) */
} TourStep;

/* The menus through steps, every tour_stride-th frame written as tour-NNNN.png; the first fade_in frames rise from the
 * background (a cut's soft landing). */
static void tour_menu(const char *dir, const TourStep *steps, size_t count, int fade_in, int fade_out, int *frame,
                      int *written)
{
    char path[512];
    int local = 0, total = 0;
    for (size_t i = 0; i < count; i++)
        total += steps[i].wait + 1;
    for (size_t i = 0; i < count; i++)
        for (int f = 0; f <= steps[i].wait; f++, (*frame)++, local++) {
            InputState in;
            memset(&in, 0, sizeof in);
            in.connected[0] = true;
            in.pressed[0] = f == 0 ? steps[i].button : 0;
            in.buttons[0] = in.pressed[0];
            gfx_frame_begin(W, H);
            menu_update(&in);
            if (*frame % tour_stride) {
                menu_draw(W, H);
                continue;
            }
            clear();
            gfx_frame_begin(W, H);
            menu_draw(W, H);
            const int left = total - local;
            uint32_t alpha = 0;
            if (local < fade_in)
                alpha = 255u * (uint32_t)(fade_in - local) / (uint32_t)fade_in;
            else if (left <= fade_out)
                alpha = 255u * (uint32_t)(fade_out - left + 1) / (uint32_t)fade_out;
            if (alpha)
                gfx_rect(0.0f, 0.0f, W, H, 0.0f, alpha << 24 | 0x0E0E10);
            snprintf(path, sizeof path, "%s/tour-%04d.png", dir, (*written)++);
            write_png(path);
        }
}

/* A title card for the tour's start and end: the logo easing in and growing a little, the title and the lines under
 * it, all rising from the background and sinking back into it at the end. frames long; lines may be NULL. */
static void tour_card(const char *dir, int frames, const char *line1, const char *line2, int *frame, int *written)
{
    enum { IN = 26, OUT = 22 };
    char path[512];
    for (int f = 0; f < frames; f++, (*frame)++) {
        gfx_frame_begin(W, H);
        if (*frame % tour_stride)
            continue;
        clear();
        const float t = f < IN ? (float)f / IN : 1.0f;
        const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);   /* out-cubic */
        const float size = 112.0f * (0.86f + 0.14f * ease), cx = W / 2.0f, top = 214.0f;
        gfx_logo(cx - size / 2.0f, top + (112.0f - size) / 2.0f, size);
        const int32_t ty = (int32_t)top + 140;
        const int32_t tw = gfx_text_width("Moonlight 360", 3), mw = gfx_text_width("Moonlight ", 3);
        gfx_text((int32_t)cx - tw / 2, ty, 3, 0xFFF4F4F5, "Moonlight");
        gfx_text((int32_t)cx - tw / 2 + mw, ty, 3, 0xFFFFA45E, "360");
        if (line1)
            gfx_text_center((int32_t)cx, ty + 82, 1, 0xFFB3B3B9, line1);
        if (line2)
            gfx_text_center((int32_t)cx, ty + 110, 1, 0xFF74747C, line2);
        /* The fades: the whole card from the background, and back. Text comes a beat after the logo. */
        const int left = frames - f;
        float hide = f < IN ? 1.0f - ease : left < OUT ? 1.0f - (float)left / OUT : 0.0f;
        gfx_rect(0.0f, ty - 10.0f, W, 200.0f, 0.0f,
                 (uint32_t)(255.0f * (f < IN + 8 ? (f < 8 ? 1.0f : 1.0f - (float)(f - 8) / IN) : hide)) << 24 |
                     0x0E0E10);
        gfx_rect(0.0f, 0.0f, W, H, 0.0f, (uint32_t)(255.0f * hide) << 24 | 0x0E0E10);
        snprintf(path, sizeof path, "%s/tour-%04d.png", dir, (*written)++);
        write_png(path);
    }
}

/* Connecting: the first time's pairing with its PIN, then the connection's steps; then the game with the detailed
 * stats, their graphs running on. */
static void tour_connect(const char *dir, int *frame, int *written)
{
    static const char *const step_names[] = { "Reach PC", "Pair", "Start app", "Stream", "Picture" };
    static const char *const details[] = { "Contacting 192.168.1.100...", "Paired.",
                                           "Starting \"Red Dead Redemption 2\" at 1920x1080 60 fps, 30000 kbps...",
                                           "RTSP handshake...", "Waiting for the first frame from the PC\xE2\x80\xA6" };
    static NetStats net = { .rtt_ms = 1, .budget_ms = 1000.0f / 60, .count = NET_HISTORY };
    for (int k = 0; k < NET_HISTORY; k++)
        net.interval_ms[k] = 1000.0f / 60 + (float)((k * 37) % 5) - 2.0f;
    static HudHistory hist = { .latency_count = HUD_HISTORY, .load_count = HUD_HISTORY };
    const HudStats hud = { .ready = true, .fps = 60, .target_fps = 60, .width = 1920, .height = 1080, .cabac = 0,
                           .stream_kind = STREAM_LEAN, .host_ms = 4, .net_ms = 4, .decode_ms = 12, .display_ms = 14 };
    PerfReadings perf;
    perf_get(&perf);
    /* Reach PC; the PIN, held; the other steps, quick; the stream, held, fading out at its end. */
    /* The pairing with its PIN first; then the connection, Reach PC done, each step after it in turn; the stream. */
    enum { PAIRING = 110, STEP = 26, CONNECT = PAIRING + 4 * STEP, STREAM = 190, FADE = 18 };
    char path[512];
    for (int f = 0; f < CONNECT + STREAM; f++, (*frame)++) {
        gfx_frame_begin(W, H);
        const bool streaming = f >= CONNECT;
        if (streaming)
            for (int k = 0; k < HUD_HISTORY; k++) {
                const int q = k + f - CONNECT + 40;
                hist.latency[k][0] = 4.0f;
                hist.latency[k][1] = 3.0f + (q % 17 == 0 ? 3.0f : 0.0f);
                hist.latency[k][2] = 8.0f + (float)((q * 29) % 9) * 0.4f;
                hist.latency[k][3] = 12.0f + (float)((q * 7) % 5);
                hist.cpu[k] = (uint8_t)(58 + (q * 13) % 11);
                hist.cpu_peak[k] = (uint8_t)(84 + (q * 5) % 10);
                hist.gpu[k] = (uint8_t)(14 + (q * 11) % 6);
            }
        if (*frame % tour_stride)
            continue;
        clear();
        if (streaming) {
            fake_game();
            gfx_frame_begin(W, H);
            ui_draw_stats(STATS_DETAILED, HUD_TOP_LEFT, HUD_MEDIUM, &hud, &perf, &net, &hist);
            const int left = CONNECT + STREAM - f;
            if (left < FADE)   /* out to the background, for the menus to rise from */
                gfx_rect(0.0f, 0.0f, W, H, 0.0f, (255u * (uint32_t)(FADE - left) / FADE) << 24 | 0x0E0E10);
        } else if (f < PAIRING) {
            ui_draw_status(W, H, "Pair with your PC",
                           "Open Sunshine's web UI on the PC, go to the PIN page and enter this code. "
                           "Name the device Xbox 360. This is needed once.",
                           "4827", true);
        } else {
            const int step = 1 + (f - PAIRING) / STEP;
            ui_draw_progress(W, H, "Red Dead Redemption 2",
                             "on 192.168.1.100  \xC2\xB7  1920\xC3\x97" "1080  \xC2\xB7  60 fps  \xC2\xB7  30 Mbps",
                             step_names, 5, step, details[step]);
        }
        snprintf(path, sizeof path, "%s/tour-%04d.png", dir, (*written)++);
        write_png(path);
    }
}

/* ---- the tour's desktop: mouse mode ---- */

/* The desktop's wallpaper, made once: deep blue into violet, with a warm glow and a cool one. */
static float wallpaper[H][W][3];

static void make_wallpaper(void)
{
    static const float top_left[3] = { 0.07f, 0.11f, 0.24f }, bottom_right[3] = { 0.20f, 0.08f, 0.26f };
    static const float warm[3] = { 0.85f, 0.38f, 0.30f }, cool[3] = { 0.16f, 0.62f, 0.66f };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float u = ((float)x / W + (float)y / H) / 2.0f;
            const float wx = ((float)x - 930.0f) / 360.0f, wy = ((float)y - 210.0f) / 240.0f;
            const float cx = ((float)x - 330.0f) / 420.0f, cy = ((float)y - 600.0f) / 260.0f;
            const float w = 0.42f * expf(-(wx * wx + wy * wy)), c = 0.30f * expf(-(cx * cx + cy * cy));
            for (int k = 0; k < 3; k++)
                wallpaper[y][x][k] = top_left[k] + (bottom_right[k] - top_left[k]) * u + warm[k] * w + cool[k] * c;
        }
}

/* An arrow pointer with its tip at (x, y), white edged in black, straight into the picture. */
static void draw_pointer(float x, float y)
{
    static const float arrow[][2] = { { 0, 0 }, { 0, 17 }, { 4.2f, 13.2f }, { 7.2f, 19.6f }, { 9.6f, 18.5f },
                                       { 6.6f, 12.2f }, { 12.2f, 12.2f } };
    enum { POINTS = sizeof arrow / sizeof arrow[0] };
    const float size = 1.35f;
    for (int py = (int)y - 3; py < (int)y + 32; py++)
        for (int px = (int)x - 3; px < (int)x + 22; px++) {
            if (px < 0 || py < 0 || px >= W || py >= H)
                continue;
            float fill = 0.0f, edge = 0.0f;   /* 4x4 samples a pixel: inside the arrow, and within 1.4 px of it */
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float qx = ((float)px + (sx + 0.5f) / 4.0f - x) / size, qy = ((float)py + (sy + 0.5f) / 4.0f - y) / size;
                    bool in = false, near = false;
                    for (int i = 0, j = POINTS - 1; i < POINTS; j = i++) {
                        const float *a = arrow[i], *b = arrow[j];
                        if ((a[1] > qy) != (b[1] > qy) && qx < (b[0] - a[0]) * (qy - a[1]) / (b[1] - a[1]) + a[0])
                            in = !in;
                        const float ex = b[0] - a[0], ey = b[1] - a[1];
                        float t = ((qx - a[0]) * ex + (qy - a[1]) * ey) / (ex * ex + ey * ey);
                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                        const float dx = qx - a[0] - t * ex, dy = qy - a[1] - t * ey;
                        if (dx * dx + dy * dy < 1.0f / (size * size))
                            near = true;
                    }
                    fill += in && !near ? 1.0f / 16 : 0.0f;
                    edge += in || near ? 1.0f / 16 : 0.0f;
                }
            for (int k = 0; k < 3; k++) {
                const float under = fb[py][px][k] * (1.0f - edge) + 0.04f * edge;   /* the black edge */
                fb[py][px][k] = under * (1.0f - fill) + 0.98f * fill;
            }
        }
}

/* argb with its alpha times a. */
static uint32_t faded(uint32_t argb, float a)
{
    return (uint32_t)((float)(argb >> 24) * clampf(a, 0, 1) + 0.5f) << 24 | (argb & 0x00FFFFFF);
}

/* Text at 80%, centred on cx. */
static void small_text_center(float cx, float y, uint32_t argb, const char *text)
{
    gfx_scale_begin(0.8f, cx, y);
    gfx_text_center((int32_t)cx, (int32_t)y, 1, argb, text);
    gfx_scale_end();
}

enum { ICON_X = 34, ICON_Y = 30, ICON_STEP = 104, TASKBAR_H = 52 };

/* A desktop icon: a folder, or a tile with a mark on it; its name under it. */
static void desktop_icon(int i, bool selected)
{
    static const char *const names[] = { "Games", "Photos", "Notes" };
    const float x = ICON_X, y = (float)(ICON_Y + i * ICON_STEP);
    if (selected)
        gfx_rect(x - 14, y - 10, 92, 98, 10, 0x40FFFFFF);
    if (i == 0) {
        gfx_rect(x, y + 2, 28, 14, 4, 0xFFC98A1F);    /* the folder's tab */
        gfx_rect(x, y + 8, 64, 46, 7, 0xFFF2B33D);
        gfx_rect(x, y + 18, 64, 36, 7, 0xFFF7C45A);
    } else {
        gfx_rect(x + 6, y + 2, 52, 52, 12, i == 1 ? 0xFF2FAF98 : 0xFF5B6CF0);
        if (i == 1) {
            gfx_circle(x + 22, y + 20, 6, 0xFFFFF2C2);
            gfx_rect(x + 14, y + 34, 36, 10, 4, 0xFF1D7E6D);
        } else
            for (int l = 0; l < 3; l++)
                gfx_rect(x + 18, y + 16 + 10 * l, l == 2 ? 18 : 28, 4, 2, 0xFFE8EBFF);
    }
    small_text_center(x + 32, y + 64, 0xFFF4F4F5, names[i]);
}

/* The window the Games folder opens, grown to share of its size (0 to 1), as faint as alpha; hover, a cover the
 * pointer is on (-1 none). */
static void games_window(float share, float alpha, int hover)
{
    static GfxImage *covers[4];
    static const int apps[4] = { 2, 3, 4, 5 };   /* fake_apps: the games with covers */
    for (int i = 0; i < 4; i++)
        if (!covers[i] && apps_cover(apps[i]))
            covers[i] = gfx_image_create_rounded(apps_cover(apps[i]), APPS_COVER_W, APPS_COVER_H, 12.0f);
    enum { WX = 330, WY = 110, WW = 640, WH = 300 };
    gfx_scale_begin(0.9f + 0.1f * share, WX + WW / 2.0f, WY + WH / 2.0f);
    gfx_shadow(WX, WY, WW, WH, 12, 28, faded(0x90000000, alpha));
    gfx_rect(WX, WY, WW, WH, 12, faded(0xFF34343C, alpha));
    gfx_rect(WX + 1, WY + 1, WW - 2, WH - 2, 11, faded(0xFF1C1C22, alpha));
    gfx_rect(WX + 1, WY + 1, WW - 2, 46, 11, faded(0xFF26262D, alpha));
    gfx_rect(WX + 1, WY + 30, WW - 2, 17, 0, faded(0xFF26262D, alpha));
    gfx_rect(WX + 20, WY + 15, 22, 11, 3, faded(0xFFF2B33D, alpha));   /* a little folder by the name */
    gfx_text(WX + 54, WY + 12, 1, faded(0xFFF4F4F5, alpha), "Games");
    for (int k = 0; k < 3; k++)   /* minimise, maximise, close */
        gfx_rect(WX + WW - 112 + 36 * k, WY + 22, 16, k == 0 ? 2 : 12, k == 0 ? 1 : 3,
                 faded(k == 2 ? 0xFFE0645A : 0xFF8A8A94, alpha));
    for (int i = 0; i < 4; i++) {
        const float cx = WX + 34 + 146.0f * (float)i, cy = WY + 76;
        if (i == hover)
            gfx_rect(cx - 8, cy - 8, 136, 222, 12, faded(0xFF3A3A44, alpha));
        if (covers[i])
            gfx_image(covers[i], cx, cy, 120, 160, faded(0xFFFFFFFF, alpha));
        char name[40];
        gfx_fit_width(name, sizeof name, fake_apps[apps[i]], 1, 150);
        small_text_center(cx + 60, cy + 174, faded(i == hover ? 0xFFF4F4F5 : 0xFFB3B3B9, alpha), name);
    }
    gfx_scale_end();
}

/* The taskbar: a few app tiles in the middle, the Games window's marked while open, a clock on the right. */
static void taskbar(bool games_open)
{
    gfx_rect(0, H - TASKBAR_H, W, TASKBAR_H, 0, 0xE0121217);
    static const uint32_t tiles[] = { 0xFF5B6CF0, 0xFFF2B33D, 0xFF2FAF98, 0xFFE0569B, 0xFF8A8A94 };
    const float left = W / 2.0f - 2.5f * 48.0f;
    for (int i = 0; i < 5; i++) {
        const float x = left + 48.0f * (float)i + 6;
        gfx_rect(x, H - TASKBAR_H + 9, 34, 34, 9, tiles[i]);
        if (i == 1 && games_open)
            gfx_rect(x + 11, H - 7, 12, 3, 1.5f, 0xFFFFA45E);
    }
    gfx_scale_begin(0.8f, W - 30, H - TASKBAR_H / 2.0f);
    gfx_text_right(W - 30, H - TASKBAR_H / 2 - 10, 1, 0xFFE4E4E7, "21:47");
    gfx_scale_end();
}

static float ease_in_out(float t)
{
    t = clampf(t, 0, 1);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
}

/* Mouse mode, on a stream of the PC's desktop: the stream menu, Mouse and keys, Mouse mode on; then the pointer
 * crosses to the Games folder, opens it with a double click, and rests on a game. */
static void tour_desktop(const char *dir, int *frame, int *written)
{
    /* When each thing happens, in frames from the start. */
    enum { FADE = 18, MENU = 40, DOWN = 74, PAGE = 102, MOUSE = 146, GO = 172, ARRIVE = 222, CLICK1 = 230,
           CLICK2 = 240, OPENED = 256, GO2 = 286, ARRIVE2 = 330, END = 392 };
    make_wallpaper();
    overlay_set_menu_combo(INPUT_GAMEPAD_BACK, "Back");
    int stats_mode = 0;
    const float rest_x = 760, rest_y = 380, icon_x = ICON_X + 30, icon_y = ICON_Y + 26;
    const float cover_x = 330 + 34 + 146.0f * 2 + 64, cover_y = 110 + 76 + 92;
    char path[512];
    for (int f = 0; f < END; f++, (*frame)++) {
        InputState in;
        memset(&in, 0, sizeof in);
        in.connected[0] = true;
        in.pressed[0] = f == DOWN ? INPUT_GAMEPAD_DPAD_DOWN : f == PAGE || f == MOUSE ? INPUT_GAMEPAD_A : 0;
        in.buttons[0] = in.pressed[0];
        gfx_frame_begin(W, H);
        if (f == MENU)
            overlay_open();
        overlay_update(&in, &stats_mode);
        if (*frame % tour_stride)
            continue;
        /* The pointer: resting; to the folder; still while it's opened; on to a game. */
        float px = rest_x, py = rest_y;
        if (f >= GO) {
            const float t = ease_in_out((float)(f - GO) / (ARRIVE - GO));
            px = rest_x + (icon_x - rest_x) * t;
            py = rest_y + (icon_y - rest_y) * t - 40.0f * sinf(t * 3.14159f);   /* a slight arc, as a hand moves */
        }
        if (f >= GO2) {
            const float t = ease_in_out((float)(f - GO2) / (ARRIVE2 - GO2));
            px = icon_x + (cover_x - icon_x) * t;
            py = icon_y + (cover_y - icon_y) * t - 30.0f * sinf(t * 3.14159f);
        }
        memcpy(fb, wallpaper, sizeof fb);
        gfx_frame_begin(W, H);
        for (int i = 0; i < 3; i++)
            desktop_icon(i, i == 0 && f >= CLICK1);
        if (f >= CLICK2) {
            const float t = clampf((float)(f - CLICK2) / (OPENED - CLICK2), 0, 1);
            const float grow = 1 - (1 - t) * (1 - t) * (1 - t);
            games_window(grow, t * 1.6f, f >= ARRIVE2 - 6 ? 2 : -1);
        }
        taskbar(f >= CLICK2);
        /* Each click: a ring spreading from the pointer. */
        for (int c = 0; c < 2; c++) {
            const int at = c ? CLICK2 : CLICK1;
            if (f >= at && f < at + 14) {
                const float t = (float)(f - at) / 14.0f;
                gfx_circle(px, py, 6 + 16 * t, faded(0x60FFFFFF, 1 - t));
            }
        }
        gfx_flush();
        if (f < MENU || f >= MOUSE)
            draw_pointer(px, py);
        overlay_draw(W, H, "Desktop", stats_mode);
        const int left = END - f;
        const float hide = f < FADE ? 1.0f - (float)f / FADE : left < FADE ? 1.0f - (float)left / FADE : 0.0f;
        if (hide > 0)
            gfx_rect(0.0f, 0.0f, W, H, 0.0f, (uint32_t)(255.0f * hide) << 24 | 0x0E0E10);
        snprintf(path, sizeof path, "%s/tour-%04d.png", dir, (*written)++);
        write_png(path);
    }
    overlay_set_mouse(false);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    gfx_init();
    if (argc > 2 && !strcmp(argv[2], "tour")) {
        /* A walk through the app for the README: tour-NNNN.png, 60 a second, or 30 with "tour 2". */
        if (argc > 3)
            tour_stride = atoi(argv[3]) > 0 ? atoi(argv[3]) : 1;
        static Config c = { .host = "192.168.1.100", .app = "Red Dead Redemption 2", .width = 1920, .height = 1080,
                            .fps = 60, .bitrate_kbps = 30000, .decode = true, .draw = true, .audio_packet_ms = 20,
                            .packet_size = 1392, .slices = 24, .app_id = 3, .low_latency = true, .stats_mode = 1,
                            .host_mac = "a8:5e:45:1c:2b:7f",
                            .ui_sounds = true, .rumble = true, .show_recent = true,
                            .recent_id = { 3, 6, 4, 5 },
                            .recent_name = { "Red Dead Redemption 2", "Cyberpunk 2077", "Grand Theft Auto VI",
                                             "Forza Horizon 6" } };
        menu_open(&c, "moonlight.ini");
        int frame = 0, written = 0;
        /* Intro: the title; the menus' fade at the end leads back into it as the animation loops. */
        tour_card(dir, 110, NULL, NULL, &frame, &written);
        /* Act one: home, the cursor on Start streaming, A; connecting, the PIN, the stream. */
        static const TourStep open[] = { { 0, 70 }, { INPUT_GAMEPAD_A, 20 } };
        tour_menu(dir, open, sizeof open / sizeof open[0], 16, 0, &frame, &written);
        tour_connect(dir, &frame, &written);
        /* Mouse mode, on the PC's desktop. */
        tour_desktop(dir, &frame, &written);
        /* Act two: the features, one a beat, building to the themes' colour; it ends there, fading into the title as
         * the animation loops. */
        static const TourStep menus[] = {
            { 0, 44 },                                                            /* Play: the moon */
            { INPUT_GAMEPAD_DPAD_RIGHT, 22 }, { INPUT_GAMEPAD_DPAD_RIGHT, 16 },   /* Recent */
            { INPUT_GAMEPAD_DPAD_RIGHT, 16 }, { INPUT_GAMEPAD_B, 16 },
            { INPUT_GAMEPAD_DPAD_DOWN, 12 }, { INPUT_GAMEPAD_A, 40 },             /* the covers */
            { INPUT_GAMEPAD_DPAD_RIGHT, 14 }, { INPUT_GAMEPAD_DPAD_RIGHT, 14 },
            { INPUT_GAMEPAD_DPAD_LEFT, 14 }, { INPUT_GAMEPAD_DPAD_LEFT, 18 },
            { INPUT_GAMEPAD_B, 16 },
            { INPUT_GAMEPAD_RIGHT_SHOULDER, 26 },                                 /* Video: up to 30 Mbps */
            { INPUT_GAMEPAD_DPAD_DOWN, 12 }, { INPUT_GAMEPAD_DPAD_DOWN, 12 },
            { INPUT_GAMEPAD_DPAD_LEFT, 12 }, { INPUT_GAMEPAD_DPAD_LEFT, 16 },
            { INPUT_GAMEPAD_DPAD_RIGHT, 12 }, { INPUT_GAMEPAD_DPAD_RIGHT, 32 },
            { INPUT_GAMEPAD_RIGHT_SHOULDER, 30 },                                 /* Network: Wake the PC on */
            { INPUT_GAMEPAD_A, 34 },
            { INPUT_GAMEPAD_RIGHT_SHOULDER, 20 },                                 /* Other: the themes */
            { INPUT_GAMEPAD_DPAD_DOWN, 7 }, { INPUT_GAMEPAD_DPAD_DOWN, 7 }, { INPUT_GAMEPAD_DPAD_DOWN, 7 },
            { INPUT_GAMEPAD_DPAD_DOWN, 7 }, { INPUT_GAMEPAD_DPAD_DOWN, 7 }, { INPUT_GAMEPAD_DPAD_DOWN, 20 },
            { INPUT_GAMEPAD_DPAD_RIGHT, 26 }, { INPUT_GAMEPAD_DPAD_RIGHT, 26 },
            { INPUT_GAMEPAD_DPAD_RIGHT, 26 }, { INPUT_GAMEPAD_DPAD_RIGHT, 60 },   /* the last beat: held, then out */
        };
        tour_menu(dir, menus, sizeof menus / sizeof menus[0], 16, 28, &frame, &written);
        printf("%d frames\n", written);
        return 0;
    }
    if (argc > 4 && !strcmp(argv[2], "sky")) {
        /* The Play tab's sky from argv[3] to argv[4] seconds (the clock run on to the start without drawing; an event
         * starts on the first frame it's due), every quarter second as sky-SSS.ss.png: for the rare events. */
        static Config sky = { .host = "192.168.1.100", .app = "Desktop", .width = 1280, .height = 720, .fps = 30,
                              .bitrate_kbps = 8000, .decode = true, .draw = true, .audio_packet_ms = 20,
                              .packet_size = 1392, .slices = 6 };
        menu_open(&sky, "moonlight.ini");
        const int from = (int)(atof(argv[3]) * 60.0), to = (int)(atof(argv[4]) * 60.0);
        for (int f = 0; f < from; f++)
            gfx_frame_begin(W, H);
        moon_skip((float)from / 60.0f);   /* the sky's clock runs only while it's drawn */
        for (int f = from; f < to; f++) {
            if (f % 15) {
                gfx_frame_begin(W, H);
                menu_draw(W, H);
                continue;
            }
            char name[32];
            snprintf(name, sizeof name, "sky-%06.2f", f / 60.0);
            shot_menu(dir, name);
        }
        return 0;
    }
    if (argc > 3 && !strcmp(argv[2], "moon")) {
        /* The Play tab's sky over time: every third frame (20 a second) of argv[3] seconds, as moon-NNNN.png. */
        static Config sky = { .host = "192.168.1.100", .app = "Desktop", .width = 1280, .height = 720, .fps = 30,
                              .bitrate_kbps = 8000, .decode = true, .draw = true, .audio_packet_ms = 20,
                              .packet_size = 1392, .slices = 6 };
        menu_open(&sky, "moonlight.ini");
        const int frames = atoi(argv[3]) * 60;
        for (int f = 0; f < frames; f++) {
            if (f % 3) {
                gfx_frame_begin(W, H);
                menu_draw(W, H);   /* the scene keeps its own time and events */
                continue;
            }
            char name[32];
            snprintf(name, sizeof name, "moon-%04d", f / 3);
            shot_menu(dir, name);
        }
        return 0;
    }
    static Config config = { .host = "192.168.1.100", .app = "Desktop", .width = 1920, .height = 1080, .fps = 60,
                             .bitrate_kbps = 20000, .fast_decode = false, .decode = true, .draw = true,
                             .audio_packet_ms = 20, .packet_size = 1392, .slices = 24, .app_id = 3,
                             .recent_id = { 3, 6, 4, 5 }, .show_recent = true,
                             .recent_name = { "Red Dead Redemption 2", "Cyberpunk 2077", "Grand Theft Auto VI",
                                              "Forza Horizon 6" } };
    snprintf(config.app, sizeof config.app, "Red Dead Redemption 2");
    menu_open(&config, "moonlight.ini");
    shot_menu(dir, "1-play");
    press(INPUT_GAMEPAD_DPAD_DOWN);
    shot_menu(dir, "2-play-app");
    press(INPUT_GAMEPAD_DPAD_DOWN);    /* Host PC */
    press(INPUT_GAMEPAD_DPAD_DOWN);    /* onto Recent */
    press(INPUT_GAMEPAD_DPAD_RIGHT);
    for (int i = 0; i < 20; i++) {   /* the ring settles */
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "2b-play-recent");
    press(INPUT_GAMEPAD_DPAD_UP);      /* back to Host PC */
    press(INPUT_GAMEPAD_DPAD_UP);      /* App */
    menu_set_message("Could not reach Sunshine at 192.168.1.100:\ncURL error: Could not connect to server (-5)\n"
                     "Check the Host PC setting.");
    shot_menu(dir, "3-play-error");
    menu_set_message("No app named \"Elden Ring\" on the host.\nApps: Desktop, Steam Big Picture, Hades II, Forza "
                     "Horizon 5, Stardew Valley, Hollow Knight, Balatro, Cyberpunk 2077, Baldur's Gate 3, Red Dead "
                     "Redemption 2, The Witcher 3, Celeste, Dead Cells, Hollow Knight Silksong, Sea of Thieves\n"
                     "Change the App setting.");
    shot_menu(dir, "3b-play-error-long");
    menu_set_message(NULL);
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_DPAD_DOWN);   /* 1080p picture */
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);   /* Bitrate */
    for (int i = 0; i < 12; i++) {   /* past the tab's fade */
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "4-video");
    press(INPUT_GAMEPAD_DPAD_UP);   /* Frame rate, at 1080p and 60 fps: the lean-stream notice */
    shot_menu(dir, "4b-video-warning");
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);   /* Stats */
    press(INPUT_GAMEPAD_Y);   /* details on */
    shot_menu(dir, "4c-video-stats-details");
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);   /* Stats size, the last row: the tab scrolls */
    for (int i = 0; i < 20; i++) {
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "4d-video-scrolled");
    press(INPUT_GAMEPAD_DPAD_UP);
    press(INPUT_GAMEPAD_DPAD_UP);
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    shot_menu(dir, "5-network");
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_DPAD_UP);   /* Logging, the last row */
    for (int i = 0; i < 12; i++) {   /* past the tab's fade */
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "5b-other");
    press(INPUT_GAMEPAD_DPAD_UP);      /* up from Logging to Theme */
    press(INPUT_GAMEPAD_DPAD_UP);
    press(INPUT_GAMEPAD_DPAD_RIGHT);   /* Blue */
    for (int i = 0; i < 12; i++) {
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "5d-theme-blue");
    press(INPUT_GAMEPAD_DPAD_LEFT);    /* back to Orange */
    press(INPUT_GAMEPAD_DPAD_DOWN);    /* and down to Logging */
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    for (int i = 0; i < 12; i++) {   /* past the tab's fade */
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "5c-about");
    config.logging = true;
    press(INPUT_GAMEPAD_Y);   /* the log */
    shot_menu(dir, "5e-about-log");
    press(INPUT_GAMEPAD_Y);
    config.logging = false;
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);   /* back to Play */
    press(INPUT_GAMEPAD_DPAD_DOWN);        /* Host PC */
    press(INPUT_GAMEPAD_A);                /* the finder */
    for (int i = 0; i < 12; i++) {
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "6b-finder");
    press(INPUT_GAMEPAD_B);
    press(INPUT_GAMEPAD_DPAD_UP);          /* App again */
    press(INPUT_GAMEPAD_A);                /* the picker */
    for (int i = 0; i < 12; i++) {   /* covers made, the row settled: on the current app */
        gfx_frame_begin(W, H);
        menu_draw(W, H);
    }
    shot_menu(dir, "6-picker");
    press(INPUT_GAMEPAD_B);

    clear();
    gfx_frame_begin(W, H);
    ui_draw_status(W, H, "Pair with your PC",
                   "Open Sunshine's web UI on the PC, go to the PIN page and enter this code. "
                   "Name the device Xbox 360. This is needed once.",
                   "4827", true);
    char path[512];
    snprintf(path, sizeof path, "%s/7-pairing.png", dir);
    write_png(path);

    clear();
    gfx_frame_begin(W, H);
    static const char *const steps[] = { "Reach PC", "Pair", "Start app", "Stream", "Picture" };
    for (int i = 0; i < 40; i++) {   /* the track's fill slides into place */
        clear();
        gfx_frame_begin(W, H);
        ui_draw_progress(W, H, "Desktop",
                         "on 192.168.1.100  \xC2\xB7  1920x1080  \xC2\xB7  60 fps  \xC2\xB7  20 Mbps", steps, 5, 3,
                         "RTSP handshake...");
    }
    snprintf(path, sizeof path, "%s/8-connecting.png", dir);
    write_png(path);

    clear();
    fake_game();
    overlay_open();
    gfx_frame_begin(W, H);
    PerfReadings perf;
    perf_get(&perf);
    /* Figures from a real 1080p60 session with a lean stream, six minutes at 30 Mbit/s (log 2026-11-24 00:04:
     * 59 fps, latency 40 ms = PC 4 + net 4 + decode 12 + display 20), and a troubled one. */
    static NetStats net = { .rtt_ms = 1, .rtt_var_ms = 0, .budget_ms = 1000.0f / 60, .count = NET_HISTORY };
    for (int i = 0; i < NET_HISTORY; i++)
        net.interval_ms[i] = 1000.0f / 60 + (float)((i * 37) % 5) - 2.0f;
    const HudStats hud = { .ready = true, .fps = 59, .target_fps = 60, .width = 1920, .height = 1080, .cabac = 0,
                           .stream_kind = STREAM_LEAN, .host_ms = 4, .net_ms = 4, .decode_ms = 12,
                           .display_ms = 20 };
    static HudHistory hist = { .latency_count = HUD_HISTORY, .load_count = HUD_HISTORY };
    for (int i = 0; i < HUD_HISTORY; i++) {
        const float wobble = (float)((i * 29) % 9) - 4.0f;
        hist.latency[i][0] = 4.0f;
        hist.latency[i][1] = 4.0f + (i % 13 == 0 ? 3.0f : 0.0f);
        hist.latency[i][2] = 12.0f + wobble * 0.5f;
        hist.latency[i][3] = 18.0f + (float)((i * 7) % 5);
        hist.cpu[i] = (uint8_t)(62 + (i * 13) % 9);
        hist.cpu_peak[i] = (uint8_t)(88 + (i * 5) % 8);
        hist.gpu[i] = (uint8_t)(16 + (i * 11) % 5);
    }
    hist.latency[64][2] = 24.0f;   /* a slow frame */
    ui_draw_stats(STATS_COMPACT, HUD_TOP_LEFT, HUD_MEDIUM, &hud, &perf, &net, &hist);
    overlay_draw(W, H, "Desktop", STATS_COMPACT);
    snprintf(path, sizeof path, "%s/9-stream-menu.png", dir);
    write_png(path);

    clear();
    fake_game();
    gfx_frame_begin(W, H);
    ui_draw_stats(STATS_DETAILED, HUD_TOP_LEFT, HUD_MEDIUM, &hud, &perf, &net, &hist);
    snprintf(path, sizeof path, "%s/10-stats.png", dir);
    write_png(path);

    clear();
    fake_game();
    gfx_frame_begin(W, H);
    ui_draw_stats(STATS_COMPACT, HUD_TOP_LEFT, HUD_MEDIUM, &hud, &perf, &net, &hist);
    snprintf(path, sizeof path, "%s/11-stats-compact.png", dir);
    write_png(path);

    static NetStats bad;
    bad = net;
    bad.rtt_ms = 18;
    bad.loss_pct = 1.7f;
    bad.lost_frames = 28;
    bad.interval_ms[52] = 81.0f;
    bad.interval_ms[60] = 70.0f;
    bad.lost_before[71] = true;
    bad.lost_before[72] = true;
    const HudStats troubled = { .ready = true, .fps = 26, .target_fps = 30, .width = 1600, .height = 900, .cabac = 0,
                                .host_ms = 5, .net_ms = 14, .decode_ms = 29, .display_ms = 25, .dropped = 3,
                                .audio_gaps = 4 };
    clear();
    fake_game();
    gfx_frame_begin(W, H);
    ui_draw_stats(STATS_DETAILED, HUD_BOTTOM_RIGHT, HUD_SMALL, &troubled, &perf, &bad, &hist);
    snprintf(path, sizeof path, "%s/12-stats-trouble.png", dir);
    write_png(path);

    static Config empty = { .app = "Desktop", .width = 1280, .height = 720, .fps = 30, .bitrate_kbps = 8000,
                            .decode = true, .draw = true, .audio_packet_ms = 20, .packet_size = 1392, .slices = 24 };
    menu_open(&empty, "moonlight.ini");
    shot_menu(dir, "0-welcome");

    clear();   /* the logo, big and at its real size, for judging it */
    gfx_frame_begin(W, H);
    gfx_logo(80.0f, 80.0f, 400.0f);
    gfx_logo(560.0f, 80.0f, 160.0f);
    gfx_logo(560.0f, 280.0f, 48.0f);
    ui_draw_header();
    snprintf(path, sizeof path, "%s/13-logo.png", dir);
    write_png(path);
    return 0;
}
