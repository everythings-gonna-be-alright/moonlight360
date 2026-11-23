#version 450
// A full-screen quad corner: clip-space position and texture coordinate.
layout(location = 0) in vec2 a_position;
layout(location = 2) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;

void main()
{
	gl_Position = vec4(a_position, 0.0, 1.0);
	v_uv = a_uv;
}
