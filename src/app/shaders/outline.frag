// Outlines above the DS's resolution (app/hardware_3d.h), after edges.frag
// and nearby.frag, once for each scene (not for each picture the scene is
// put into): for each pixel of an opaque polygon less than u_outline.x
// pixels from an edge of its own polygon number, its number's edge colour
// (red, green, blue) and how much of the pixel the outline covers (alpha:
// as far as the pixel is within that distance, so soft on the inner side);
// elsewhere 0. compose.frag mixes it over the scene's pixel, then fog. So
// an outline is as wide on every side, on the nearer of two groups only,
// and never inside a group.
//
// The lines nearest first, so that the search ends early; a pixel far from
// every edge (nearby.frag) costs two reads.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_outline;

// Red: the number, times 63; alpha 1 where opaque (scene.frag).
layout(set = 2, binding = 0) uniform sampler2D u_numbers;
// edges.frag's: a bit for each pixel, 16 pixels across to each of its own.
layout(set = 2, binding = 1) uniform sampler2D u_edges;
// nearby.frag's: 1 (red) for each block of 16 by 16 pixels with an edge
// less than u_outline.x pixels away.
layout(set = 2, binding = 2) uniform sampler2D u_nearby;
layout(set = 3, binding = 0) uniform Outline {
  uvec4 u_outline;  // x: the outline's width in pixels.
  // Where edges.frag looked, in the scene's pixels: left, top, right and
  // bottom, the last two past the last. This pass draws only there.
  uvec4 u_bounds;
  vec4 u_colours[8];  // The edge colour of each group of 8 polygon numbers.
};

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

void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const ivec4 bounds = ivec4(u_bounds);
  o_outline = vec4(0.0);
  if (texelFetch(u_nearby, p >> 4, 0).r < 0.5) return;
  const vec4 me = texelFetch(u_numbers, p, 0);
  if (me.a < 0.5) return;
  const uint number = uint(me.r * 63.0 + 0.5);
  const int reach = int(u_outline.x) - 1;
  const int from = max(p.x - reach, bounds.x), to = min(p.x + reach, bounds.z - 1);
  int nearest = 1 << 20;
  for (int dy = 0; dy <= reach && dy * dy < nearest; ++dy) {
    if (p.y - dy >= bounds.y) nearest = nearest_edge(p, p.y - dy, number, from, to, nearest);
    if (dy != 0 && p.y + dy < bounds.w) nearest = nearest_edge(p, p.y + dy, number, from, to, nearest);
  }
  const float cover = clamp(float(u_outline.x) - sqrt(float(nearest)), 0.0, 1.0);
  if (cover > 0.0) o_outline = vec4(u_colours[number >> 3].rgb, cover);
}
