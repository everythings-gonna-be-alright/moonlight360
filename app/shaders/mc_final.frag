#version 450
// H.264 luma inter prediction plus the residual, four pixels a texel, from the planes src/gpumc.c draws first (mc_pg/pb/ph/pj.frag):
// each quarter position is one plane's value or the rounded average of two (dev/bench/source/mc_ref.c, mc_ref_luma). The
// block's vector puts x0 anywhere, so each plane is read two texels at a time and the five samples from x0 picked
// out with weights; no branches (xost gives every branch registers of its own).
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_g;      // the planes: RGBA8, tiled, four samples a texel, margins
layout(set = 0, binding = 1) uniform sampler2D u_b;
layout(set = 0, binding = 2) uniform sampler2D u_h;
layout(set = 0, binding = 3) uniform sampler2D u_j;
layout(set = 0, binding = 4) uniform sampler2D u_grid;   // R32F, a vector per 4x4 luma block (h264_xenon.h)
layout(set = 0, binding = 5) uniform sampler2D u_res;    // the residual: 16-bit signed, four samples a texel
layout(set = 0, binding = 6) uniform sampler2D u_flags;  // R8, a byte a macroblock: 1 if it has a residual
layout(push_constant) uniform Push {
	vec4 plane_size;  // 1 / plane width (texels), 1 / plane height
	vec4 grid_size;   // 1 / grid width, 1 / grid height
	vec4 res_size;    // 1 / residual width (texels), 1 / height, 1 / flags width, 1 / flags height
	mat4 order;       // pixels 0-3 to the output channels (read as columns: xost compiles mat4 * vec4 to nothing)
} pc;

vec4 texel(sampler2D s, float tx, float row)
{
	return floor(texture(s, vec2(tx + 0.5, row + 0.5) * pc.plane_size.xy) * 255.0 + 0.5);
}

// Samples k .. k+3 and k+1 .. k+4 of the eight in texels a, b, w being k one-hot.
vec4 pick(vec4 a, vec4 b, vec4 w)
{
	return vec4(dot(a, w), dot(vec4(a.yzw, b.x), w), dot(vec4(a.zw, b.xy), w), dot(vec4(a.w, b.xyz), w));
}

vec4 pick1(vec4 a, vec4 b, vec4 w)
{
	return vec4(dot(vec4(a.yzw, b.x), w), dot(vec4(a.zw, b.xy), w), dot(vec4(a.w, b.xyz), w), dot(b, w));
}

vec4 average(vec4 a, vec4 b)
{
	return floor((a + b + 1.0) / 2.0);
}

float is_value(float v, float k)
{
	return 1.0 - min(abs(v - k), 1.0);
}

void main()
{
	float tx = floor(v_pixel.x);
	float y = floor(v_pixel.y);
	float packed = floor(texture(u_grid, vec2(tx + 0.5, floor(y / 4.0) + 0.5) * pc.grid_size.xy).r + 0.5);
	float high = floor(packed / 4096.0);
	float mvx = high - 2048.0;
	float mvy = packed - high * 4096.0 - 2048.0;
	float ix = floor(mvx / 4.0);
	float iy = floor(mvy / 4.0);
	float fx = mvx - ix * 4.0;
	float fy = mvy - iy * 4.0;
	// x0 in plane samples (the margin is 4), as a texel and the sample within it; rows likewise.
	float px = tx * 4.0 + ix + 4.0;
	float qt = floor(px / 4.0);
	float k = px - qt * 4.0;
	vec4 w = vec4(is_value(k, 0.0), is_value(k, 1.0), is_value(k, 2.0), is_value(k, 3.0));
	float row = y + iy + 4.0;

	vec4 ga = texel(u_g, qt, row), gb = texel(u_g, qt + 1.0, row);
	vec4 G = pick(ga, gb, w), H = pick1(ga, gb, w);
	vec4 M = pick(texel(u_g, qt, row + 1.0), texel(u_g, qt + 1.0, row + 1.0), w);
	vec4 b = pick(texel(u_b, qt, row), texel(u_b, qt + 1.0, row), w);
	vec4 s = pick(texel(u_b, qt, row + 1.0), texel(u_b, qt + 1.0, row + 1.0), w);
	vec4 ha = texel(u_h, qt, row), hb = texel(u_h, qt + 1.0, row);
	vec4 h = pick(ha, hb, w), m = pick1(ha, hb, w);
	vec4 j = pick(texel(u_j, qt, row), texel(u_j, qt + 1.0, row), w);

	float x0w = is_value(fx, 0.0), x1w = is_value(fx, 1.0), x2w = is_value(fx, 2.0), x3w = is_value(fx, 3.0);
	float y0w = is_value(fy, 0.0), y1w = is_value(fy, 1.0), y2w = is_value(fy, 2.0), y3w = is_value(fy, 3.0);
	vec4 p = x0w * y0w * G + x1w * y0w * average(G, b) + x2w * y0w * b + x3w * y0w * average(b, H) +
	         x0w * y1w * average(G, h) + x0w * y2w * h + x0w * y3w * average(h, M) +
	         x2w * y1w * average(b, j) + x2w * y2w * j + x2w * y3w * average(j, s) +
	         x1w * y2w * average(h, j) + x3w * y2w * average(j, m) +
	         x1w * y1w * average(b, h) + x1w * y3w * average(h, s) +
	         x3w * y1w * average(b, m) + x3w * y3w * average(m, s);
	// The residual, where the macroblock has one: prediction + residual, clipped, as FFmpeg's IDCTs add it.
	vec4 res = floor(texture(u_res, vec2(tx + 0.5, y + 0.5) * pc.res_size.xy) * 32767.0 + 0.5);
	float flag = floor(texture(u_flags, vec2(floor(tx / 4.0) + 0.5, floor(y / 16.0) + 0.5) * pc.res_size.zw).r * 255.0 + 0.5);
	p = clamp(p + flag * res, 0.0, 255.0);
	vec4 q = p / 255.0;
	o_color = q.x * pc.order[0] + q.y * pc.order[1] + q.z * pc.order[2] + q.w * pc.order[3];
}
