// Two uses, by u_capture.x:
//
// A screen (0): the main engine's screen with the 3D scene put in, or a kept
// display capture: where the screen is marked (nds::DisplayEngine::kMark3D,
// here the alpha byte), a pixel of it is mixed as the mark says, then given
// the master brightness.
//
// A display capture (1), into a bank of 256 by 256 pixels (nds::
// CaptureRecipe): for each of its pixels that the capture writes, the first
// source (the main engine's picture with the scene put in, or the scene
// alone), the second (a bank), or the two blended.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_colour;

layout(set = 2, binding = 0) uniform sampler2D u_screen;  // 256 by 192; alpha: the mark.
layout(set = 2, binding = 1) uniform sampler2D u_scene;   // Colour times alpha, and alpha.
layout(set = 2, binding = 2) uniform sampler2D u_second;  // For a capture: the second source's bank.
layout(set = 3, binding = 0) uniform Compose {
  vec4 u_brightness;  // x: 1 towards white, 2 towards black, else none; y: how far, 0 to 1.
  // x: 0 a screen, 1 a capture. y: for a screen, the lines u_scene has (192,
  // or 256 for a kept capture).
  vec4 u_options;
  // For a capture: x: the source (0 to 2), plus 4 for the 3D alone; y: the
  // two weights, the second times 256; z: width, and height times 65536;
  // w: where it starts in its bank, and where the second source starts in
  // its bank times 65536.
  uvec4 u_capture;
};

vec3 brightened(vec3 c, uint way, float factor) {
  if (way == 2u) return c + (1.0 - c) * factor;
  if (way == 3u) return c - c * factor;
  return c;
}

// The screen at `at` with what u_scene holds at `inside` put in.
vec3 composed(vec2 at, vec2 inside, bool lit) {
  vec4 screen = texture(u_screen, at);
  uint mark = uint(screen.a * 255.0 + 0.5);
  if ((mark & 128u) == 0u) return screen.rgb;
  vec4 scene = texture(u_scene, inside);
  if (scene.a < 0.5 / 63.0) return screen.rgb;
  vec3 c = scene.rgb / scene.a;
  uint effect = mark & 3u;
  c = brightened(c, effect, float((mark >> 2) & 31u) / 16.0);
  if (lit) c = brightened(c, uint(u_brightness.x + 0.5) + 1u, u_brightness.y);
  return effect == 1u ? mix(screen.rgb, c, scene.a) : c;
}

void main() {
  o_colour = vec4(0.0, 0.0, 0.0, 1.0);
  if (u_capture.x == 0u && u_options.x < 0.5) {
    o_colour.rgb = composed(v_place, vec2(v_place.x, v_place.y * 192.0 / u_options.y), true);
    return;
  }
  // The bank's pixel, and where in it (0 to 1 each way).
  vec2 bank = v_place * 256.0;
  vec2 pixel = min(floor(bank), vec2(255.0));
  vec2 within = bank - pixel;
  uint n = uint(pixel.y) * 256u + uint(pixel.x);
  uint width = u_capture.z & 0xFFFFu, height = u_capture.z >> 16;
  uint i = (n - (u_capture.w & 0xFFFFu)) & 0xFFFFu;
  if (i / width >= height) discard;
  vec2 at = (vec2(float(i % width), float(i / width)) + within) / vec2(256.0, 192.0);
  vec3 first;
  if ((u_capture.x & 4u) != 0u) {
    vec4 scene = texture(u_scene, at);
    first = scene.a < 0.5 / 63.0 ? vec3(0.0) : scene.rgb / scene.a;
  } else {
    first = composed(at, at, false);
  }
  uint j = ((u_capture.w >> 16) + i) & 0xFFFFu;
  vec3 second = texture(u_second, (vec2(float(j % 256u), float(j / 256u)) + within) / 256.0).rgb;
  uint source = u_capture.x & 3u;
  if (source == 0u) o_colour.rgb = first;
  else if (source == 1u) o_colour.rgb = second;
  else o_colour.rgb = min(first * float(u_capture.y & 31u) / 16.0 + second * float((u_capture.y >> 8) & 31u) / 16.0, vec3(1.0));
}
