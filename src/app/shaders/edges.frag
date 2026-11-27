// Outlines above the DS's resolution (app/hardware_3d.h): the edges of the
// scene's opaque polygons, from the scene pass's polygon numbers and
// depths, as bits: each pixel of this pass holds 16 pixels of a line of the
// scene, red the first 8 (the lowest bit the leftmost), green the next 8.
// The outline pass (outline.frag) looks through these for the nearest edge
// of a pixel's own polygon number.
//
// An opaque pixel on its polygon's edge (scene.frag: a side of the polygon
// less than a DS pixel away) is an edge where a pixel next to it (across or
// down; past the picture's edge, the clear plane) has another polygon
// number and is further away, as edge marking goes on the DS: so the edge
// is on the nearer side only, inside a group of one number there is none,
// and where two polygons go through each other there is none but near
// their sides. A gap narrower
// than a DS pixel with the same number on its far side does not count: the
// triangles leave such holes where the DS's spans do not. A pixel that is not
// opaque costs one read.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_bits;

layout(set = 2, binding = 0) uniform sampler2D u_fog_marks;  // The depth (24 bits in red, green, blue).
layout(set = 2, binding = 1) uniform sampler2D u_numbers;    // Red: the number, times 63; green 1 on its edge; alpha 1 where opaque.
layout(set = 3, binding = 0) uniform Edges {
  // x: the outline's width in pixels; y and z: the clear plane's polygon
  // number and depth; w: how many pixels a DS pixel is wide.
  uvec4 u_outline;
};

ivec2 g_size;

uint depth_at(ivec2 p) {
  const uvec3 b = uvec3(texelFetch(u_fog_marks, p, 0).rgb * 255.0 + 0.5);
  return b.r << 16 | b.g << 8 | b.b;
}

bool inside(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, g_size)); }

// The polygon number at `p`: the clear plane's past the picture's edge.
uint number_at(ivec2 p) { return inside(p) ? uint(texelFetch(u_numbers, p, 0).r * 63.0 + 0.5) : u_outline.y; }

// Whether the opaque pixel `p` (of `number`) is an edge towards `way`,
// where the pixel there has `other`.
bool edge_towards(ivec2 p, ivec2 way, uint number, uint other) {
  if (other == number) return false;
  const ivec2 q = p + way;
  if (depth_at(p) >= (inside(q) ? depth_at(q) : u_outline.z)) return false;
  // A gap inside the group: the same number again within a DS pixel.
  for (int j = 1; j < int(u_outline.w); ++j) {
    const ivec2 r = q + j * way;
    if (!inside(r)) break;
    if (number_at(r) == number) return false;
  }
  return true;
}

void main() {
  g_size = textureSize(u_numbers, 0);
  const ivec2 first = ivec2(int(gl_FragCoord.x) * 16, int(gl_FragCoord.y));
  uint bits = 0u;
  uint left = number_at(first - ivec2(1, 0));
  vec4 me = texelFetch(u_numbers, first, 0);
  for (int i = 0; i < 16; ++i) {
    const ivec2 p = first + ivec2(i, 0);
    const uint number = uint(me.r * 63.0 + 0.5);
    const vec4 next = inside(p + ivec2(1, 0)) ? texelFetch(u_numbers, p + ivec2(1, 0), 0)
                                              : vec4(float(u_outline.y) / 63.0, 0.0, 0.0, 0.0);
    if (me.a > 0.5 && me.g > 0.5) {
      const uint right = uint(next.r * 63.0 + 0.5);
      if (edge_towards(p, ivec2(-1, 0), number, left) || edge_towards(p, ivec2(1, 0), number, right) ||
          edge_towards(p, ivec2(0, -1), number, number_at(p - ivec2(0, 1))) ||
          edge_towards(p, ivec2(0, 1), number, number_at(p + ivec2(0, 1))))
        bits |= 1u << uint(i);
    }
    left = number;
    me = next;
  }
  o_bits = vec4(float(bits & 255u), float(bits >> 8), 0.0, 0.0) / 255.0;
}
