// Outlines above the DS's resolution (app/hardware_3d.h), after edges.frag:
// for each block of 16 by 16 pixels of the scene, 1 (red) where an edge is
// less than the outline's width away from the block, else 0. The compose
// pass (compose.frag) looks for the nearest edge only in such blocks, so a
// pixel far from every edge costs one read.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_near;

layout(set = 2, binding = 0) uniform sampler2D u_edges;  // edges.frag's bits, 16 pixels across to each.
layout(set = 3, binding = 0) uniform Nearby {
  uvec4 u_outline;  // x: the outline's width in pixels.
  // Where edges.frag looked, in the scene's pixels: left, top, right and
  // bottom, the last two past the last. u_edges holds nothing outside.
  uvec4 u_bounds;
};

uint edge_bits(int group, int y) {
  const vec2 b = texelFetch(u_edges, ivec2(group, y), 0).rg * 255.0 + 0.5;
  return uint(b.r) | uint(b.g) << 8;
}

void main() {
  const ivec2 block = ivec2(gl_FragCoord.xy);
  const int reach = int(u_outline.x) - 1;
  const int first_group = int(u_bounds.x) >> 4, last_group = (int(u_bounds.z) - 1) >> 4;
  // Of the groups to each side, only the pixels within reach.
  const uint left_mask = reach > 0 ? 0xFFFFu << uint(16 - reach) & 0xFFFFu : 0u;
  const uint right_mask = (1u << uint(reach)) - 1u;
  const int top = max(block.y * 16 - reach, int(u_bounds.y)), bottom = min(block.y * 16 + 15 + reach, int(u_bounds.w) - 1);
  uint near = 0u;
  for (int y = top; y <= bottom && near == 0u; ++y) {
    if (block.x >= first_group && block.x <= last_group) near |= edge_bits(block.x, y);
    if (left_mask != 0u && block.x - 1 >= first_group) near |= edge_bits(block.x - 1, y) & left_mask;
    if (right_mask != 0u && block.x + 1 <= last_group) near |= edge_bits(block.x + 1, y) & right_mask;
  }
  o_near = vec4(near != 0u ? 1.0 : 0.0, 0.0, 0.0, 0.0);
}
