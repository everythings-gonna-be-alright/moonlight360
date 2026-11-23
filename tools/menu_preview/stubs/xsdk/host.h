/* Host stand-ins for the xsdk calls the menu code makes: enough to draw it into a picture on a PC
 * (tools/menu_preview). Not the SDK. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t NTSTATUS;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS 0

enum {
    INPUT_GAMEPAD_DPAD_UP = 0x0001, INPUT_GAMEPAD_DPAD_DOWN = 0x0002, INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
    INPUT_GAMEPAD_DPAD_RIGHT = 0x0008, INPUT_GAMEPAD_START = 0x0010, INPUT_GAMEPAD_BACK = 0x0020,
    INPUT_GAMEPAD_LEFT_THUMB = 0x0040, INPUT_GAMEPAD_RIGHT_THUMB = 0x0080, INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
    INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200, INPUT_GAMEPAD_GUIDE = 0x0400, INPUT_GAMEPAD_A = 0x1000,
    INPUT_GAMEPAD_B = 0x2000, INPUT_GAMEPAD_X = 0x4000, INPUT_GAMEPAD_Y = 0x8000,
};
typedef struct {
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
} INPUT_GAMEPAD;

/* gpu.h */
enum { XSDK_PRIMITIVE_TRIANGLE_STRIP = 6 };
enum { XSDK_BLEND_ZERO = 0, XSDK_BLEND_ONE = 1, XSDK_BLEND_SRC_ALPHA = 6, XSDK_BLEND_ONE_MINUS_SRC_ALPHA = 7 };
enum { XSDK_BLEND_OP_ADD };
enum { XSDK_WRITE_ALL = 0xF };
enum { XSDK_CULL_NONE };
typedef struct { uint32_t color_source, color_dest, color_op, alpha_source, alpha_dest, alpha_op, write_mask; bool alpha_to_coverage; } XsdkBlend;
typedef struct { uint32_t x; } XsdkBlendState;
typedef struct { bool depth_test; } XsdkDepthStencil;
typedef struct { uint32_t x; } XsdkDepthStencilState;
typedef struct { uint32_t cull; } XsdkRaster;
typedef struct { uint32_t x; } XsdkRasterState;
volatile uint32_t *xsdk_gpu_alloc(uint32_t size_bytes);
void xsdk_gpu_free(volatile uint32_t *memory);
uint32_t xsdk_gpu_address(volatile const uint32_t *memory);
uint32_t xsdk_gpu_fence(void);
NTSTATUS xsdk_gpu_wait(uint32_t fence, uint32_t timeout_ms);
void xsdk_gpu_invalidate(uint32_t address, uint32_t size_bytes);
void xsdk_gpu_blend_state(XsdkBlendState *state, const XsdkBlend *setting);
void xsdk_gpu_depth_stencil_state(XsdkDepthStencilState *state, const XsdkDepthStencil *setting);
void xsdk_gpu_raster_state(XsdkRasterState *state, const XsdkRaster *setting);
void xsdk_gpu_set_blend(const XsdkBlendState *state);
void xsdk_gpu_set_depth_stencil(const XsdkDepthStencilState *state);
void xsdk_gpu_set_raster(const XsdkRasterState *state);
void xsdk_gpu_set_vertex_constants(uint32_t first, const float *values, uint32_t count);
void xsdk_gpu_set_vertices(uint32_t stream, uint32_t address, uint32_t size_bytes);
void xsdk_gpu_draw(uint32_t primitive_type, uint32_t vertex_count);

/* shader.h */
enum { XSDK_USAGE_POSITION = 0, XSDK_USAGE_TEXCOORD = 5, XSDK_USAGE_COLOR = 10 };
enum { XSDK_VERTEX_FLOAT1, XSDK_VERTEX_FLOAT2, XSDK_VERTEX_FLOAT3, XSDK_VERTEX_FLOAT4 };
typedef struct { uint32_t usage, usage_index, format, stream, offset_bytes; uint8_t components[4]; bool per_instance; } XsdkVertexElement;
typedef struct { uint32_t x; } XsdkShader;
typedef struct { const void *container; uint32_t container_bytes; } XsdkBuiltShader;
NTSTATUS xsdk_shader_load(XsdkShader *shader, const void *container, uint32_t bytes);
void xsdk_shader_free(XsdkShader *shader);
NTSTATUS xsdk_shader_set_vertex_layout(XsdkShader *shader, const XsdkVertexElement *elements, uint32_t count, const uint32_t *strides);
void xsdk_gpu_set_shaders(const XsdkShader *vertex, const XsdkShader *pixel);
void xsdk_built_shader_set_literals(const XsdkBuiltShader *built);

/* text.h */
enum { XSDK_TEXT_CELL_WIDTH = 10, XSDK_TEXT_CELL_HEIGHT = 20 };
void xsdk_text_draw(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
void xsdk_text_measure(const char *utf8, uint32_t scale, uint32_t *width, uint32_t *height);

/* xam.h */
typedef struct { uint32_t x[8]; } XAM_OVERLAPPED;
enum { ERROR_SUCCESS = 0, ERROR_IO_INCOMPLETE = 996, ERROR_IO_PENDING = 997 };
uint32_t xsdk_xam_keyboard(uint32_t user, const uint16_t *title, const uint16_t *description, const uint16_t *def,
                           uint16_t *text, uint32_t chars, XAM_OVERLAPPED *overlapped);
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *overlapped, uint32_t timeout_ms, uint32_t *result);
