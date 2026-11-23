#version 450
// A corner of the quad a motion compensation pass draws over its whole target: its position in pixels, and the same
// position passed on, so a pixel's centre gets its column and row plus one half (src/gpumc.c).
layout(location = 0) in vec2 a_position;
layout(location = 2) in vec2 a_pixel;
layout(location = 0) out vec2 v_pixel;
layout(push_constant) uniform Push {
	vec4 to_clip;   // x, y: scale; z, w: offset (pixels to clip space, y up)
} pc;

void main()
{
	gl_Position = vec4(a_position * pc.to_clip.xy + pc.to_clip.zw, 0.0, 1.0);
	v_pixel = a_pixel;
}
