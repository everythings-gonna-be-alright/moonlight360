#version 450
// An image (RGBA), tinted: its colour times the tint, so the tint's alpha fades it and its colour dims it.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_atlas;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_image;

void main()
{
	o_color = v_color * texture(u_image, v_atlas);
}
