#version 450
// G: the integer samples themselves.
// One of the hybrid decoder's luma planes (src/gpumc.c): MV-independent, the whole reference picture with a margin of
// 4 samples on each side (one texel left and right, 4 rows above and below) where H.264's clamping makes every value
// further out equal the margin's, so the final pass reads exact values for any vector. Four samples a texel.
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_ref;    // the reference plane, R8, point sampled, clamped
layout(push_constant) uniform Push {
	vec4 ref_size;    // width, height, 1 / width, 1 / height
} pc;

float sample_at(float x, float y)
{
	return floor(texture(u_ref, vec2(x + 0.5, y + 0.5) * pc.ref_size.zw).r * 255.0 + 0.5);
}

vec4 quad_at(float x, float y)
{
	return vec4(sample_at(x, y), sample_at(x + 1.0, y), sample_at(x + 2.0, y), sample_at(x + 3.0, y));
}

vec4 taps6(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f)
{
	return a - 5.0 * b + 20.0 * c + 20.0 * d - 5.0 * e + f;
}

// The horizontal 6-tap sums of the four samples from x, row y.
vec4 row_taps(float x, float y)
{
	float c0 = sample_at(x - 2.0, y);
	float c1 = sample_at(x - 1.0, y);
	float c2 = sample_at(x, y);
	float c3 = sample_at(x + 1.0, y);
	float c4 = sample_at(x + 2.0, y);
	float c5 = sample_at(x + 3.0, y);
	float c6 = sample_at(x + 4.0, y);
	float c7 = sample_at(x + 5.0, y);
	float c8 = sample_at(x + 6.0, y);
	return taps6(vec4(c0, c1, c2, c3), vec4(c1, c2, c3, c4), vec4(c2, c3, c4, c5), vec4(c3, c4, c5, c6),
	             vec4(c4, c5, c6, c7), vec4(c5, c6, c7, c8));
}

void main()
{
	float x = floor(v_pixel.x) * 4.0 - 4.0;
	float y = floor(v_pixel.y) - 4.0;
	vec4 p;
	p = quad_at(x, y);
	o_color = p / 255.0;
}
