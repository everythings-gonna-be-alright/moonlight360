/*
 * Draws the app's menus on a PC, into PNG files, from the same menu.c, ui.c, overlay.c and gfx.c the console runs.
 * The GPU calls are stood in for by a software rasteriser that does what shape.frag and xsdk's text do: rounded
 * rectangles by signed distance, blended by alpha, and the 10x20 bitmap font point-sampled.
 *
 *     tools/menu_preview/build.sh      (writes the PNGs to build/preview)
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
#include "overlay.h"
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
NTSTATUS xsdk_shader_set_vertex_layout(XsdkShader *s, const XsdkVertexElement *e, uint32_t c, const uint32_t *st) { (void)s, (void)e, (void)c, (void)st; return 0; }
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel) { (void)vertex, (void)pixel; }
void xsdk_built_shader_set_literals(const XsdkBuiltShader *built) { (void)built; }

static float word_float(uint32_t w)
{
    union { uint32_t u; float f; } c = { .u = w };
    return c.f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The strip gfx.c writes: corners top left, top right, bottom left, bottom right; shape.frag per pixel. */
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count)
{
    (void)primitive_type, (void)vertex_count;
    const volatile uint32_t *v = ring + (vertices_address - RING_BASE) / 4;
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

/* ---- keyboard, config and app list stand-ins ---- */

uint32_t xsdk_xam_keyboard(uint32_t user, const uint16_t *t, const uint16_t *d, const uint16_t *def, uint16_t *text,
                           uint32_t chars, XAM_OVERLAPPED *o)
{
    (void)user, (void)t, (void)d, (void)def, (void)text, (void)chars, (void)o;
    return 1;
}
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *o, uint32_t timeout_ms, uint32_t *result) { (void)o, (void)timeout_ms, (void)result; return ERROR_SUCCESS; }
bool config_save(const char *path, const Config *config) { (void)path, (void)config; return true; }

static const char *const fake_apps[] = { "Desktop", "Steam Big Picture", "Hades II", "Forza Horizon 5",
                                         "Elden Ring", "Stardew Valley", "Hollow Knight", "Balatro" };
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
            fb[y][x][0] = 0x0C / 255.0f;   /* the resolve's clear colour, main.c CLEAR_ARGB */
            fb[y][x][1] = 0x10 / 255.0f;
            fb[y][x][2] = 0x16 / 255.0f;
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
static void fake_game(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            fb[y][x][0] = 0.25f + 0.35f * (float)x / W;
            fb[y][x][1] = 0.35f + 0.25f * sinf((float)y / 90.0f);
            fb[y][x][2] = 0.20f + 0.30f * (float)(W - x) / W;
        }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    gfx_init();
    static Config config = { .host = "192.168.1.100", .app = "Desktop", .width = 1280, .height = 720, .fps = 30,
                             .bitrate_kbps = 8000, .fast_decode = true, .decode = true, .draw = true,
                             .audio_packet_ms = 20, .packet_size = 1392 };
    menu_open(&config, "moonlight.ini");
    shot_menu(dir, "1-play");
    press(INPUT_GAMEPAD_DPAD_DOWN);
    shot_menu(dir, "2-play-app");
    menu_set_message("Could not reach Sunshine at 192.168.1.100:\ncURL error: Could not connect to server (-5)\n"
                     "Check the Host PC setting.");
    shot_menu(dir, "3-play-error");
    menu_set_message(NULL);
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    shot_menu(dir, "4-video");
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_DPAD_DOWN);
    shot_menu(dir, "5-network");
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);
    press(INPUT_GAMEPAD_RIGHT_SHOULDER);   /* back to Play, App still selected */
    press(INPUT_GAMEPAD_A);                /* the picker */
    press(INPUT_GAMEPAD_DPAD_DOWN);
    press(INPUT_GAMEPAD_DPAD_DOWN);
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
    ui_draw_status(W, H, "Connecting", "Connecting: RTSP handshake...", NULL, true);
    snprintf(path, sizeof path, "%s/8-connecting.png", dir);
    write_png(path);

    clear();
    fake_game();
    overlay_open();
    gfx_frame_begin(W, H);
    PerfReadings perf;
    perf_get(&perf);
    ui_draw_stats("decode 30 fps (9.5 ms)  shown 29 fps  dropped 0  errors 0  audio gaps 0  CABAC  fast decode", &perf);
    overlay_draw(W, H, "Desktop");
    snprintf(path, sizeof path, "%s/9-stream-menu.png", dir);
    write_png(path);

    clear();
    fake_game();
    gfx_frame_begin(W, H);
    ui_draw_stats("decode 30 fps (9.5 ms)  shown 29 fps  dropped 0  errors 0  audio gaps 0  CABAC  fast decode", &perf);
    snprintf(path, sizeof path, "%s/10-stats.png", dir);
    write_png(path);

    static Config empty = { .app = "Desktop", .width = 1280, .height = 720, .fps = 30, .bitrate_kbps = 8000,
                            .decode = true, .draw = true, .audio_packet_ms = 20, .packet_size = 1392 };
    menu_open(&empty, "moonlight.ini");
    shot_menu(dir, "0-welcome");
    return 0;
}
