#version 450
// Calibration (src/gpumc.c): the channels 1, 2, 3 and 4 out of 255, to find which memory byte each lands in after a
// resolve; and the grid's value at the texel's block, to check it arrives as written.
layout(location = 0) in vec2 v_pixel;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 1) uniform sampler2D u_grid;
layout(push_constant) uniform Push {
	vec4 ref_size;
	vec4 grid_size;
	mat4 order;
} pc;

void main()
{
	float packed = floor(texture(u_grid, vec2(floor(v_pixel.x) + 0.5, floor(v_pixel.y) + 0.5) * pc.grid_size.xy).r + 0.5);
	float high = floor(packed / 4096.0);
	if (floor(v_pixel.y) == 0.0)
		o_color = vec4(1.0, 2.0, 3.0, 4.0) / 255.0;
	else
		o_color = vec4(high - 2048.0 + 128.0, packed - high * 4096.0 - 2048.0 + 128.0, 0.0, 255.0) / 255.0;
}
