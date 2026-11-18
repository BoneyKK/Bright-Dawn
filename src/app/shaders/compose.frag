// The main engine's screen with the 3D scene put in: where the screen is
// marked (nds::DisplayEngine::kMark3D, here the alpha byte), a pixel of the
// scene is mixed as the mark says, then given the master brightness.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_colour;

layout(set = 2, binding = 0) uniform sampler2D u_screen;  // 256 by 192; alpha: the mark.
layout(set = 2, binding = 1) uniform sampler2D u_scene;   // Colour times alpha, and alpha.
layout(set = 3, binding = 0) uniform Compose {
  vec4 u_brightness;  // x: 1 towards white, 2 towards black, else none; y: how far, 0 to 1.
};

vec3 brightened(vec3 c, uint way, float factor) {
  if (way == 2u) return c + (1.0 - c) * factor;
  if (way == 3u) return c - c * factor;
  return c;
}

void main() {
  vec4 screen = texture(u_screen, v_place);
  o_colour = vec4(screen.rgb, 1.0);
  uint mark = uint(screen.a * 255.0 + 0.5);
  if ((mark & 128u) == 0u) return;
  vec4 scene = texture(u_scene, v_place);
  if (scene.a < 0.5 / 63.0) return;
  vec3 c = scene.rgb / scene.a;
  uint effect = mark & 3u;
  c = brightened(c, effect, float((mark >> 2) & 31u) / 16.0);
  c = brightened(c, uint(u_brightness.x + 0.5) + 1u, u_brightness.y);
  o_colour.rgb = effect == 1u ? mix(screen.rgb, c, scene.a) : c;
}
