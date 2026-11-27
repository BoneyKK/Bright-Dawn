// The 3D scene's corners (nds::HardwareScene::Vertex), as they are, but for
// x times u_across.x: in a wide picture (app/hardware_3d.h) the DS's screen
// is the part in the middle. And for outlines, where in its triangle a
// pixel is (nds::HardwareScene::kCornerShift): 1 at the corner of each part,
// 0 on the side across from it, in proportion across the screen.
#version 450

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texel;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in uvec4 a_info;

layout(location = 0) out vec2 v_texel;
layout(location = 1) out vec4 v_colour;
layout(location = 2) flat out uvec4 v_info;
layout(location = 3) out float v_w_depth;  // The depth, taken across with perspective.
layout(location = 4) noperspective out vec3 v_corner;

layout(set = 1, binding = 0) uniform Scene { vec4 u_across; };

void main() {
  gl_Position = vec4(a_position.x * u_across.x, a_position.yzw);
  v_texel = a_texel;
  v_colour = a_colour;
  v_info = a_info;
  v_w_depth = a_position.z / a_position.w;
  const uint corner = (a_info.w >> 8) & 3u;
  v_corner = vec3(corner == 0u ? 1.0 : 0.0, corner == 1u ? 1.0 : 0.0, corner == 2u ? 1.0 : 0.0);
}
