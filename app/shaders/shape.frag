#version 450
// A rounded rectangle by its signed distance: coverage falls from 1 to 0 across the edge, one pixel wide for a crisp
// shape, many for a soft shadow. Scalars only: with vector max and length, the console filled triangles near the
// corners with nothing (hybench's shape check, 2026-11-23), which showed in the menus as rhombs.
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_local;
layout(location = 2) in vec4 v_shape;
layout(location = 0) out vec4 o_color;

void main()
{
	float qx = abs(v_local.x) - v_shape.x + v_shape.z;
	float qy = abs(v_local.y) - v_shape.y + v_shape.z;
	float ox = max(qx, 0.0);
	float oy = max(qy, 0.0);
	float outside = sqrt(ox * ox + oy * oy);
	float inside = min(max(qx, qy), 0.0);
	float d = outside + inside - v_shape.z;
	float coverage = clamp(0.5 - d * v_shape.w, 0.0, 1.0);
	o_color = vec4(v_color.r, v_color.g, v_color.b, v_color.a * coverage);
}
