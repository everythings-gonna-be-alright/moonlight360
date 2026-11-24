#version 450
// A corner of an image's quad (src/gfx.c): its position in pixels from the viewport's top left, its tint and where it
// is in the image.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_atlas;
layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_atlas;
layout(push_constant) uniform Push {
	vec4 to_clip;   // x, y: scale; z, w: offset (pixels to clip space, y up)
} pc;

void main()
{
	gl_Position = vec4(a_position * pc.to_clip.xy + pc.to_clip.zw, 0.0, 1.0);
	v_color = a_color;
	v_atlas = a_atlas;
}
