#pragma once
#include "xsdk/host.h"
/* texture.h: just what gfx.c's font atlas uses; the texels live in host memory. */
enum { XSDK_FORMAT_R8 = 2, XSDK_FORMAT_R8G8B8A8 = 13 };
enum { XSDK_FILTER_POINT, XSDK_FILTER_LINEAR };
enum { XSDK_ADDRESS_CLAMP = 2 };
typedef struct {
    uint32_t address, size_bytes, width, height, format;
    uint8_t *texels;
} XsdkTexture;
typedef struct { uint32_t mag_filter, min_filter, mip_filter, address_u, address_v, address_w; } XsdkSampler;
typedef struct { uint32_t x; } XsdkSamplerState;
NTSTATUS xsdk_texture_create(XsdkTexture *texture, uint32_t format, uint32_t width, uint32_t height, uint32_t levels,
                             bool tiled);
void xsdk_texture_upload(XsdkTexture *texture, uint32_t level, const void *data, uint32_t row_bytes);
void xsdk_texture_free(XsdkTexture *texture);
void xsdk_texture_sampler_state(XsdkSamplerState *state, const XsdkSampler *sampler);
void xsdk_gpu_set_texture(uint32_t slot, const XsdkTexture *texture, const XsdkSamplerState *sampler);
