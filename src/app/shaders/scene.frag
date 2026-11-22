// A pixel of the 3D scene: the vertex colour, mixed with a texel of the
// texture atlas as the polygon asks (modulated, as a decal, or with the toon
// table), written with its colour times its alpha; and, for edge marking,
// its polygon number and depth (24 bits), a byte each (kept for opaque
// polygons only); and, for fog, its depth (24 bits) and whether it takes
// fog (the pipelines keep the depth where the polygon keeps its depth, and
// the mark where every polygon drawn on the pixel takes fog).
#version 450

layout(location = 0) in vec2 v_texel;
layout(location = 1) in vec4 v_colour;
layout(location = 2) flat in uvec4 v_info;

layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_edge;
layout(location = 2) out vec4 o_fog;

layout(set = 2, binding = 0) uniform sampler2D u_atlas;
layout(set = 3, binding = 0) uniform Scene {
  vec4 u_toon[32];
  vec4 u_options;  // x: the alpha under which nothing is drawn; y: 1 for highlight; z: 1 for opaque.
};

// A texel coordinate inside a texture `size` texels wide: repeated (every
// other time mirrored, if asked) or held at the edge.
int wrapped(int c, int size, bool repeat, bool flip) {
  if (!repeat) return clamp(c, 0, size - 1);
  if (flip && (c & size) != 0) c = -1 - c;
  return c & (size - 1);
}

void main() {
  vec4 colour = v_colour;
  uint flags = v_info.z;
  uint mode = flags & 3u;
  vec3 toon = u_toon[clamp(int(v_colour.r * 63.0 + 0.5) / 2, 0, 31)].rgb;  // By the red part.
  bool highlight = u_options.y > 0.5;
  if ((flags & 4u) != 0u) {
    ivec2 size = ivec2(int(v_info.y & 0xFFFFu), int(v_info.y >> 16));
    ivec2 st = ivec2(floor(v_texel));
    st.x = wrapped(st.x, size.x, (flags & 8u) != 0u, (flags & 32u) != 0u);
    st.y = wrapped(st.y, size.y, (flags & 16u) != 0u, (flags & 64u) != 0u);
    ivec2 place = ivec2(int(v_info.x & 0xFFFFu), int(v_info.x >> 16));
    vec4 texel = texelFetch(u_atlas, place + st, 0);
    if (mode == 0u) {
      colour = texel * v_colour;
    } else if (mode == 1u) {
      colour = vec4(mix(v_colour.rgb, texel.rgb, texel.a), v_colour.a);
    } else if (highlight) {
      colour = vec4(min(texel.rgb * v_colour.rgb + toon, 1.0), texel.a * v_colour.a);
    } else {
      colour = vec4(texel.rgb * toon, texel.a * v_colour.a);
    }
  } else if (mode == 2u) {
    colour.rgb = highlight ? min(v_colour.rgb + toon, 1.0) : toon;
  }
  if (colour.a < u_options.x) discard;
  if (u_options.z > 0.5) colour.a = 1.0;
  o_colour = vec4(colour.rgb * colour.a, colour.a);
  uint depth = uint(clamp(gl_FragCoord.z, 0.0, 1.0) * 16777215.0 + 0.5);
  o_edge = vec4(uvec4(v_info.w & 63u, depth >> 16, (depth >> 8) & 255u, depth & 255u)) / 255.0;
  o_fog = vec4(vec3(uvec3(depth >> 16, (depth >> 8) & 255u, depth & 255u)) / 255.0, (flags & 128u) != 0u ? 1.0 : 0.0);
}
