// Two uses, by u_capture.x:
//
// A screen (0): the main engine's screen with the 3D scene put in, or a kept
// display capture: where the screen is marked (nds::DisplayEngine::kMark3D,
// here the alpha byte), a pixel of it is mixed as the mark says, then given
// the master brightness. A screen with the scene can have a kept capture
// put in too, under the scene, where marked as one's.
//
// A display capture (1), into a bank of 256 by 256 pixels (nds::
// CaptureRecipe): for each of its pixels that the capture writes, the first
// source (the main engine's picture with the scene put in, or the scene
// alone), the second (a bank), or the two blended; five bits a part, as the
// DS keeps them in video memory, and blended as nds::Display does.
//
// Where the scene is put in: its pixels as the scene pass drew them, with
// fog as the DS does it (nds::HardwareScene::fog); but outlines and
// anti-aliased edges as raster.comp and finish.comp drew them, as the
// processor does. At the DS's resolution all its pixels are theirs. Above
// it, outlines are drawn from the edges of the scene's opaque polygons
// (edges.frag; nearby.frag says which pixels are far from all): a pixel of
// an opaque polygon less than u_outline.x pixels from an edge of its own
// polygon number takes its number's edge colour, as far as it is within
// that distance (soft on the inner side), then fog. So an outline is as wide on every side, on the
// nearer of two groups only, and never inside a group.
//
// Widescreen (u_wide.x is 1): the scene is wider than the screen's 256
// columns, which are in its middle; a screen is drawn as wide as the scene,
// and at its sides it is the scene alone on the lines that show it (u_wide_rows),
// with the effect the DS's screen gives the 3D at that line's end, and the
// master brightness; elsewhere black. A capture takes the scene's middle.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_colour;

layout(set = 2, binding = 0) uniform sampler2D u_screen;  // 256 by 192; alpha: the mark.
layout(set = 2, binding = 1) uniform sampler2D u_scene;   // Colour times alpha, and alpha.
// For a capture: the second source's bank. For a screen with the scene: a
// kept capture, where u_options.z is 1.
layout(set = 2, binding = 2) uniform sampler2D u_second;
layout(set = 2, binding = 3) uniform sampler2D u_fog_marks;  // The scene's depths, and alpha 1 where fog is taken.
// The number of the opaque polygon drawn on each pixel of the scene (red,
// times 63; the clear plane's where alpha is 0).
layout(set = 2, binding = 4) uniform sampler2D u_numbers;
// edges.frag's: where the edges of the opaque polygons are, a bit for each
// pixel, 16 pixels across to each of its own.
layout(set = 2, binding = 5) uniform sampler2D u_edges;
// nearby.frag's: 1 (red) for each block of 16 by 16 pixels with an edge
// less than u_outline.x pixels away.
layout(set = 2, binding = 6) uniform sampler2D u_nearby;
// finish.comp's pixels: six bits each of red, green, blue and alpha, plus
// 1 << 24 where they are to be taken.
layout(std430, set = 2, binding = 7) readonly buffer Finished { uint finished[]; };
layout(set = 3, binding = 0) uniform Compose {
  vec4 u_brightness;  // x: 1 towards white, 2 towards black, else none; y: how far, 0 to 1.
  // x: 0 a screen, 1 a capture. y: for a screen, the lines u_scene has (192,
  // or 256 for a kept capture). z: 1 where u_second is a kept capture, put
  // in where marked as one's.
  vec4 u_options;
  // For a capture: x: the source (0 to 2), plus 4 for the 3D alone; y: the
  // two weights, the second times 256; z: width, and height times 65536;
  // w: where it starts in its bank, and where the second source starts in
  // its bank times 65536.
  uvec4 u_capture;
  // finish.comp's pixels: x and y, how many across and down; w: 1 where they
  // are the scene's (u_scene is the scene, not a kept capture).
  uvec4 u_edge;
  // Fog: x: 1 where u_scene is the scene and the frame asks for fog, plus 2
  // for the alpha only; y: the offset; z: the step.
  uvec4 u_fog;
  vec4 u_fog_colour;   // With its alpha.
  vec4 u_clear_colour; // What a pixel with no alpha in u_scene holds.
  uvec4 u_fog_table[8];
  // Outlines from the scene's pixels: x, how many pixels wide (0 for none);
  // y and z, the clear plane's polygon number and depth; w, how many pixels
  // a DS pixel is wide.
  uvec4 u_outline;
  vec4 u_outline_colours[8];
  // Where outlines can be, in u_numbers's pixels (the opaque polygons'
  // bounds): left, top, right and bottom, the last two past the last.
  // u_edges holds nothing outside.
  uvec4 u_outline_bounds;
  // Widescreen: x, 1; y, where the screen's 256 columns start, and z, how
  // much of the width they are (0 to 1); w, 1 where the sides show the
  // scene.
  vec4 u_wide;
  uvec4 u_wide_rows[2];  // The lines whose sides show the scene: a bit each.
};

uint fog_density(uint i) { return u_fog_table[i / 4u][i % 4u]; }

// The 24-bit depth a fog mark holds.
uint depth_of(vec4 mark) {
  uvec3 b = uvec3(mark.rgb * 255.0 + 0.5);
  return b.r << 16 | b.g << 8 | b.b;
}

// `scene`, the scene's pixel at `inside`, with fog, by the processor's sums
// (nds::PolygonRenderer).
vec4 fogged(vec4 scene, vec2 inside) {
  if ((u_fog.x & 1u) == 0u) return scene;
  ivec2 size = textureSize(u_fog_marks, 0);
  vec4 mark = texelFetch(u_fog_marks, clamp(ivec2(inside * vec2(size)), ivec2(0), size - 1), 0);
  if (mark.a < 0.5) return scene;
  int offset = int(depth_of(mark)) / 512 - int(u_fog.y);
  int step = int(u_fog.z);
  uint density = fog_density(0u);
  if (step == 0) {
    if (offset > 0) density = fog_density(31u);
  } else if (offset >= 0) {
    int n = offset / step - 1;
    if (n >= 31) {
      density = fog_density(31u);
    } else if (n >= 0) {
      int m = offset % step;
      density = (fog_density(uint(n) + 1u) * uint(m) + fog_density(uint(n)) * uint(step - m)) / uint(step);
    }
  }
  if (density == 127u) density = 128u;
  float k = float(density) / 128.0;
  vec3 c = scene.a > 0.0 ? scene.rgb / scene.a : u_clear_colour.rgb;
  float a = mix(scene.a, u_fog_colour.a, k);
  if ((u_fog.x & 2u) == 0u) c = mix(c, u_fog_colour.rgb, k);
  return vec4(c * a, a);
}

// Six bits as eight, as the DS's screens show them (the top two bits again
// at the bottom).
float eight_bits(uint six) { return float(six << 2 | six >> 4) / 255.0; }

// A colour as video memory keeps it: five bits a part (of the six, the top
// five).
uvec3 five_bits(vec3 c) { return uvec3(c * 255.0 + 0.5) >> 3u; }

// edges.frag's bits for the 16 pixels from 16 * `group` on line `y`.
uint edge_bits(int group, int y) {
  const vec2 b = texelFetch(u_edges, ivec2(group, y), 0).rg * 255.0 + 0.5;
  return uint(b.r) | uint(b.g) << 8;
}

// The square of the distance from `p` to the nearest edge of `number` on
// line `y`, from `from` to `to` across, if less than `nearest`; else
// `nearest`. The nearest edges on each side, up to three, are looked at.
int nearest_edge(ivec2 p, int y, uint number, int from, int to, int nearest) {
  const int dy = y - p.y, group = from >> 4, base = group << 4;
  uint bits = edge_bits(group, y);
  if (to >> 4 != group) bits |= edge_bits(group + 1, y) << 16;
  bits &= (0xFFFFFFFFu << uint(from - base)) & (0xFFFFFFFFu >> uint(31 - (to - base)));
  if (bits == 0u) return nearest;
  const uint at_or_before = (2u << uint(p.x - base)) - 1u;
  uint left = bits & at_or_before, right = bits & ~at_or_before;
  for (int i = 0; i < 3 && left != 0u; ++i) {
    const int x = base + findMSB(left), d = (p.x - x) * (p.x - x) + dy * dy;
    if (d >= nearest) break;
    if (uint(texelFetch(u_numbers, ivec2(x, y), 0).r * 63.0 + 0.5) == number) {
      nearest = d;
      break;
    }
    left &= ~(1u << uint(x - base));
  }
  for (int i = 0; i < 3 && right != 0u; ++i) {
    const int x = base + findLSB(right), d = (x - p.x) * (x - p.x) + dy * dy;
    if (d >= nearest) break;
    if (uint(texelFetch(u_numbers, ivec2(x, y), 0).r * 63.0 + 0.5) == number) {
      nearest = d;
      break;
    }
    right &= ~(1u << uint(x - base));
  }
  return nearest;
}

// The scene's pixel at `inside`: finish.comp's where it is to be taken.
vec4 scene_at(vec2 inside) {
  if (u_edge.w != 0u) {
    const ivec2 size = ivec2(u_edge.xy);
    const ivec2 p = clamp(ivec2(inside * vec2(size)), ivec2(0), size - 1);
    const uint f = finished[uint(p.y) * u_edge.x + uint(p.x)];
    if ((f & (1u << 24)) != 0u) {
      const float a = float((f >> 18) & 63u) / 63.0;
      return vec4(eight_bits(f & 63u), eight_bits((f >> 6) & 63u), eight_bits((f >> 12) & 63u), 1.0) * a;
    }
  }
  if (u_outline.x != 0u) {
    // The outline: where an edge of this pixel's own polygon number
    // (edges.frag) is less than the outline's width away, as far as it is
    // within the last pixel (soft on its inner side), over the pixel. The
    // lines nearest first, so that the search ends early.
    const ivec2 size = textureSize(u_numbers, 0);
    const ivec2 p = clamp(ivec2(inside * vec2(size)), ivec2(0), size - 1);
    const ivec4 bounds = ivec4(u_outline_bounds);
    const vec4 me = all(greaterThanEqual(p, bounds.xy)) && all(lessThan(p, bounds.zw)) ? texelFetch(u_numbers, p, 0) : vec4(0.0);
    if (me.a > 0.5 && texelFetch(u_nearby, p >> 4, 0).r > 0.5) {
      const uint number = uint(me.r * 63.0 + 0.5);
      const int reach = int(u_outline.x) - 1;
      const int from = max(p.x - reach, bounds.x), to = min(p.x + reach, bounds.z - 1);
      int nearest = 1 << 20;
      for (int dy = 0; dy <= reach && dy * dy < nearest; ++dy) {
        if (p.y - dy >= bounds.y) nearest = nearest_edge(p, p.y - dy, number, from, to, nearest);
        if (dy != 0 && p.y + dy < bounds.w) nearest = nearest_edge(p, p.y + dy, number, from, to, nearest);
      }
      const float cover = clamp(float(u_outline.x) - sqrt(float(nearest)), 0.0, 1.0);
      if (cover > 0.0) {
        const vec4 scene = texture(u_scene, inside);
        const vec3 c = mix(scene.a > 0.0 ? scene.rgb / scene.a : u_clear_colour.rgb, u_outline_colours[number >> 3].rgb, cover);
        const float a = mix(scene.a, 1.0, cover);
        return fogged(vec4(c * a, a), inside);
      }
    }
  }
  return fogged(texture(u_scene, inside), inside);
}

vec3 brightened(vec3 c, uint way, float factor) {
  if (way == 2u) return c + (1.0 - c) * factor;
  if (way == 3u) return c - c * factor;
  return c;
}

// The screen at `at` with what u_scene holds at `inside` put in.
vec3 composed(vec2 at, vec2 inside, bool lit) {
  vec4 screen = texture(u_screen, at);
  // Each part's top six bits (the lowest bit of red is a mark).
  const uvec4 bytes = uvec4(screen * 255.0 + 0.5);
  const uvec3 six = bytes.rgb >> 2u;
  vec3 below = vec3(six << 2u | six >> 4u) / 255.0;
  // Under the 3D layer, a kept capture's pixel (nds::DisplayEngine::
  // kCapturedBelow), where u_second is a kept capture.
  if ((bytes.r & 1u) != 0u && u_options.z > 0.5) {
    below = texture(u_second, vec2(at.x, at.y * 192.0 / 256.0)).rgb;
    if (lit) below = brightened(below, uint(u_brightness.x + 0.5) + 1u, u_brightness.y);
  }
  uint mark = bytes.a;
  if ((mark & 128u) == 0u && (mark & 3u) == 1u) {
    // A layer on top blended with the 3D just below it (nds::DisplayEngine::
    // kMark3DBelow): its weight in the mark, the 3D's in the low bits, as
    // DisplayEngine::blend() does, then the master brightness. Where the
    // scene has no pixel, the layer as it is.
    vec4 scene = scene_at(inside);
    vec3 c = below;
    if (scene.a >= 0.5 / 63.0) {
      const uint weight_top = (mark >> 2) & 31u;
      const uint weight_3d = ((bytes.r >> 1) & 1u) << 4 | (bytes.g & 3u) << 2 | (bytes.b & 3u);
      const uvec3 under = uvec3(scene.rgb / scene.a * 255.0 + 0.5) >> 2u;
      const uvec3 mixed = min((six * weight_top + under * weight_3d) / 16u, uvec3(63u));
      c = vec3(mixed << 2u | mixed >> 4u) / 255.0;
    }
    return lit ? brightened(c, uint(u_brightness.x + 0.5) + 1u, u_brightness.y) : c;
  }
  if ((mark & 128u) == 0u) return below;
  vec4 scene = scene_at(inside);
  if (scene.a < 0.5 / 63.0) return below;
  vec3 c = scene.rgb / scene.a;
  uint effect = mark & 3u;
  c = brightened(c, effect, float((mark >> 2) & 31u) / 16.0);
  if (lit) c = brightened(c, uint(u_brightness.x + 0.5) + 1u, u_brightness.y);
  return effect == 1u ? mix(below, c, scene.a) : c;
}

// A wide screen's side, at `at` of the screen (x outside 0 to 1) and
// `inside` the scene.
vec3 side(vec2 at, vec2 inside) {
  vec3 c = vec3(0.0);
  const uint line = min(uint(at.y * 192.0), 191u);
  const uint word = line / 32u;
  if (u_wide.w > 0.5 && ((u_wide_rows[word / 4u][word % 4u] >> (line % 32u)) & 1u) != 0u) {
    const vec4 scene = scene_at(inside);
    if (scene.a >= 0.5 / 63.0) {
      c = scene.rgb / scene.a;
      // The effect of the screen's pixel at that end of the line, where it
      // shows the 3D.
      const uint mark = uint(texture(u_screen, vec2(clamp(at.x, 0.5 / 256.0, 255.5 / 256.0), at.y)).a * 255.0 + 0.5);
      const uint effect = (mark & 128u) != 0u ? mark & 3u : 0u;
      c = brightened(c, effect, float((mark >> 2) & 31u) / 16.0);
      if (effect == 1u) c *= scene.a;
    }
  }
  return brightened(c, uint(u_brightness.x + 0.5) + 1u, u_brightness.y);
}

void main() {
  o_colour = vec4(0.0, 0.0, 0.0, 1.0);
  if (u_capture.x == 0u && u_options.x < 0.5) {
    if (u_wide.x > 0.5) {
      // The screen's 256 columns in the middle; a kept capture put in is the
      // screen's size.
      const vec2 at = vec2((v_place.x - u_wide.y) / u_wide.z, v_place.y);
      const vec2 inside = vec2(u_options.y > 200.0 ? at.x : v_place.x, v_place.y * 192.0 / u_options.y);
      o_colour.rgb = at.x < 0.0 || at.x >= 1.0 ? side(at, inside) : composed(at, inside, true);
      return;
    }
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
  // In a wide scene, its middle.
  const vec2 inside = u_wide.x > 0.5 ? vec2(u_wide.y + at.x * u_wide.z, at.y) : at;
  vec3 first;
  if ((u_capture.x & 4u) != 0u) {
    vec4 scene = scene_at(inside);
    first = scene.a < 0.5 / 63.0 ? vec3(0.0) : scene.rgb / scene.a;
  } else {
    first = composed(at, inside, false);
  }
  uint j = ((u_capture.w >> 16) + i) & 0xFFFFu;
  vec3 second = texture(u_second, (vec2(float(j % 256u), float(j / 256u)) + within) / 256.0).rgb;
  uint source = u_capture.x & 3u;
  uvec3 kept;
  if (source == 0u) {
    kept = five_bits(first);
  } else if (source == 1u) {
    kept = five_bits(second);
  } else {
    // Sixteenths of each.
    uint weight_first = min(u_capture.y & 31u, 16u), weight_second = min((u_capture.y >> 8) & 31u, 16u);
    kept = min((five_bits(first) * weight_first + five_bits(second) * weight_second) / 16u, uvec3(31u));
  }
  o_colour.rgb = vec3(eight_bits(kept.r << 1), eight_bits(kept.g << 1), eight_bits(kept.b << 1));
}
