// The 3D scene's corners (nds::HardwareScene::Vertex), as they are.
#version 450

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec2 a_texel;
layout(location = 2) in vec4 a_colour;
layout(location = 3) in uvec4 a_info;

layout(location = 0) out vec2 v_texel;
layout(location = 1) out vec4 v_colour;
layout(location = 2) flat out uvec4 v_info;
layout(location = 3) out float v_w_depth;  // The depth, taken across with perspective.

void main() {
  gl_Position = a_position;
  v_texel = a_texel;
  v_colour = a_colour;
  v_info = a_info;
  v_w_depth = a_position.z / a_position.w;
}
