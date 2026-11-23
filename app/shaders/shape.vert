#version 450
// A corner of a rounded rectangle's quad (src/gfx.c): its position in pixels from the viewport's top left, its colour,
// where it sits from the rectangle's centre, and the rectangle's shape.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_local;
layout(location = 3) in vec4 a_shape;   // half width, half height, corner radius, 1 / edge softness
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_local;
layout(location = 2) out vec4 v_shape;
layout(push_constant) uniform Push {
	vec4 to_clip;   // x, y: scale; z, w: offset (pixels to clip space, y up)
} pc;

void main()
{
	gl_Position = vec4(a_position * pc.to_clip.xy + pc.to_clip.zw, 0.0, 1.0);
	v_color = a_color;
	v_local = a_local;
	v_shape = a_shape;
}
