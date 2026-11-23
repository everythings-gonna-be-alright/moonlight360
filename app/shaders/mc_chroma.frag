#version 450
// H.264 chroma inter prediction, four pixels a texel: dev/bench/source/mc_ref.c's mc_ref_chroma. Pixels 0-1 and 2-3
// each sit under one 4x4 luma block, whose vector in quarter luma samples is eighths of a chroma sample.
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_ref;    // the reference plane, R8, point sampled, clamped
layout(set = 0, binding = 1) uniform sampler2D u_grid;   // R32F, a vector per 4x4 luma block (h264_xenon.h)
layout(set = 0, binding = 2) uniform sampler2D u_res;    // this plane's residual: 16-bit signed, four samples a texel
layout(set = 0, binding = 3) uniform sampler2D u_flags;  // R8, a byte a macroblock: 1 if it has a residual
layout(push_constant) uniform Push {
	vec4 ref_size;    // width, height, 1 / width, 1 / height
	vec4 grid_size;   // 1 / grid width, 1 / grid height
	vec4 res_size;    // 1 / residual width (texels), 1 / height, 1 / flags width, 1 / flags height
	mat4 order;       // pixels 0-3 to the output channels
} pc;

float sample_at(float x, float y)
{
	return floor(texture(u_ref, vec2(x + 0.5, y + 0.5) * pc.ref_size.zw).r * 255.0 + 0.5);
}

// Two neighbouring pixels from one block's vector: x the first's column.
vec2 pair(float x, float y, float block_x)
{
	float packed = floor(texture(u_grid, vec2(block_x + 0.5, floor(y / 2.0) + 0.5) * pc.grid_size.xy).r + 0.5);
	float high = floor(packed / 4096.0);
	float mvx = high - 2048.0;
	float mvy = packed - high * 4096.0 - 2048.0;
	float ix = floor(mvx / 8.0);
	float iy = floor(mvy / 8.0);
	float dx = mvx - ix * 8.0;
	float dy = mvy - iy * 8.0;
	float sx = x + ix;
	float sy = y + iy;
	float a0 = sample_at(sx, sy);
	float a1 = sample_at(sx + 1.0, sy);
	float a2 = sample_at(sx + 2.0, sy);
	float b0 = sample_at(sx, sy + 1.0);
	float b1 = sample_at(sx + 1.0, sy + 1.0);
	float b2 = sample_at(sx + 2.0, sy + 1.0);
	float wa = (8.0 - dx) * (8.0 - dy), wb = dx * (8.0 - dy), wc = (8.0 - dx) * dy, wd = dx * dy;
	float p0 = floor((wa * a0 + wb * a1 + wc * b0 + wd * b1 + 32.0) / 64.0);
	float p1 = floor((wa * a1 + wb * a2 + wc * b1 + wd * b2 + 32.0) / 64.0);
	return vec2(p0, p1);
}

void main()
{
	float tx = floor(v_pixel.x);
	float y = floor(v_pixel.y);
	vec2 first = pair(tx * 4.0, y, tx * 2.0);
	vec2 second = pair(tx * 4.0 + 2.0, y, tx * 2.0 + 1.0);
	vec4 res = floor(texture(u_res, vec2(tx + 0.5, y + 0.5) * pc.res_size.xy) * 32767.0 + 0.5);
	float flag = floor(texture(u_flags, vec2(floor(tx / 2.0) + 0.5, floor(y / 8.0) + 0.5) * pc.res_size.zw).r * 255.0 + 0.5);
	vec4 q = clamp(vec4(first, second) + flag * res, 0.0, 255.0) / 255.0;
	// Pixel k to the channel calibration found for byte k. Multiply-adds, not "order * q": xost compiles a mat4 times a
	// vec4 into an empty program, which hangs the GPU waiting for a colour (hybench, 2026-11-23).
	o_color = q.x * pc.order[0] + q.y * pc.order[1] + q.z * pc.order[2] + q.w * pc.order[3];
}
