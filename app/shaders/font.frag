#version 450
// The atlas's red is the glyph's coverage, anti-aliased: the colour with its alpha scaled by it.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_atlas;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_atlas;

void main()
{
	o_color = vec4(v_color.rgb, v_color.a * texture(u_atlas, v_atlas).r);
}
