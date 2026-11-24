/* PNG and JPEG covers (apps.c) through stb_image (third_party/stb, public domain), in a file of its own so its
 * warnings stay out of the rest. */
#include "image.h"

#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

/* The image's pixels in source, cropped to the destination's shape around its centre and box-filtered down (or
 * sampled up) to dw x dh. */
static void fit(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    /* The part of the source with the destination's aspect: all of one side, the middle of the other. */
    float cw = (float)sw, ch = (float)sh;
    if (cw * dh > ch * dw)
        cw = ch * dw / dh;
    else
        ch = cw * dh / dw;
    const float x0 = (sw - cw) / 2.0f, y0 = (sh - ch) / 2.0f, step_x = cw / dw, step_y = ch / dh;
    for (int y = 0; y < dh; y++) {
        const int ya = (int)(y0 + y * step_y), yb_raw = (int)(y0 + (y + 1) * step_y);
        const int yb = yb_raw > ya ? yb_raw : ya + 1;
        for (int x = 0; x < dw; x++) {
            const int xa = (int)(x0 + x * step_x), xb_raw = (int)(x0 + (x + 1) * step_x);
            const int xb = xb_raw > xa ? xb_raw : xa + 1;
            unsigned sum[4] = { 0, 0, 0, 0 }, n = 0;
            for (int sy = ya; sy < yb && sy < sh; sy++)
                for (int sx = xa; sx < xb && sx < sw; sx++, n++)
                    for (int c = 0; c < 4; c++)
                        sum[c] += src[((size_t)sy * sw + sx) * 4 + c];
            for (int c = 0; c < 4; c++)
                dst[((size_t)y * dw + x) * 4 + c] = (uint8_t)(n ? sum[c] / n : 0);
        }
    }
}

uint8_t *image_decode_fit(const uint8_t *data, size_t size, int width, int height)
{
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels)
        return NULL;
    uint8_t *out = malloc((size_t)width * height * 4);
    if (out)
        fit(pixels, w, h, out, width, height);
    stbi_image_free(pixels);
    return out;
}
