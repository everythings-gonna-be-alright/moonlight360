#version 450
// The simplest GPU decoder pass (hybench's first step): four reference samples a texel, where they are, no vector.
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_ref;
layout(set = 0, binding = 1) uniform sampler2D u_grid;
layout(push_constant) uniform Push {
	vec4 ref_size;
	vec4 grid_size;
	mat4 order;
} pc;

float sample_at(float x, float y)
{
	return floor(texture(u_ref, vec2(x + 0.5, y + 0.5) * pc.ref_size.zw).r * 255.0 + 0.5);
}

void main()
{
	float x0 = floor(v_pixel.x) * 4.0;
	float y = floor(v_pixel.y);
	vec4 p = vec4(sample_at(x0, y), sample_at(x0 + 1.0, y), sample_at(x0 + 2.0, y), sample_at(x0 + 3.0, y));
	vec4 q = p / 255.0;
	// Pixel k to the channel calibration found for byte k. Multiply-adds, not "order * q": xost compiles a mat4 times a
	// vec4 into an empty program, which hangs the GPU waiting for a colour (hybench, 2026-11-23).
	o_color = q.x * pc.order[0] + q.y * pc.order[1] + q.z * pc.order[2] + q.w * pc.order[3];
}
