#version 450
// H: the half sample below each, h = clip((tap_v + 16) >> 5).
// One of the hybrid decoder's luma planes (src/gpumc.c), read from the G plane (mc_pg.frag): the reference with its
// margins, clamped as H.264 clamps, four samples a texel, so each read is aligned and the plane's own clamping is
// exact. MV-independent: the whole picture, margins included, once a frame.
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_gp;     // the G plane: RGBA8, tiled, four samples a texel
layout(push_constant) uniform Push {
	vec4 plane_size;  // 1 / plane width (texels), 1 / plane height
} pc;

vec4 texel(float tx, float row)
{
	return floor(texture(u_gp, vec2(tx + 0.5, row + 0.5) * pc.plane_size.xy) * 255.0 + 0.5);
}

vec4 taps6(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f)
{
	return a - 5.0 * b + 20.0 * c + 20.0 * d - 5.0 * e + f;
}

// The horizontal 6-tap sums of texel tx's four samples, row "row": samples from 2 before to 3 after, in texels
// tx-1 .. tx+1.
vec4 row_taps(float tx, float row)
{
	vec4 a = texel(tx - 1.0, row), b = texel(tx, row), c = texel(tx + 1.0, row);
	return taps6(vec4(a.zw, b.xy), vec4(a.w, b.xyz), b, vec4(b.yzw, c.x), vec4(b.zw, c.xy), vec4(b.w, c.xyz));
}

void main()
{
	float tx = floor(v_pixel.x);
	float row = floor(v_pixel.y);
	vec4 p;
	vec4 v = taps6(texel(tx, row - 2.0), texel(tx, row - 1.0), texel(tx, row), texel(tx, row + 1.0),
	               texel(tx, row + 2.0), texel(tx, row + 3.0));
	p = clamp(floor((v + 16.0) / 32.0), 0.0, 255.0);
	o_color = p / 255.0;
}
