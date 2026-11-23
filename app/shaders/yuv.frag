#version 450
// YUV 4:2:0 (three R8 planes) to RGB. The CPU sets the conversion per stream (BT.601/709, limited/full range):
// rgb = y_scale * (Y - y_offset) * (1,1,1) + Cr/Cb columns times (U - 0.5, V - 0.5).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_y;
layout(set = 0, binding = 1) uniform sampler2D u_u;
layout(set = 0, binding = 2) uniform sampler2D u_v;
layout(push_constant) uniform Push {
	vec4 y_coef;  // x: scale, y: offset
	vec4 u_coef;  // rgb contribution of (U - 0.5)
	vec4 v_coef;  // rgb contribution of (V - 0.5)
} pc;

void main()
{
	float y = (texture(u_y, v_uv).r - pc.y_coef.y) * pc.y_coef.x;
	float u = texture(u_u, v_uv).r - 0.5;
	float v = texture(u_v, v_uv).r - 0.5;
	vec3 rgb = vec3(y) + pc.u_coef.rgb * u + pc.v_coef.rgb * v;
	o_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
