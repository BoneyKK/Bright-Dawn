// A pixel of the 3D scene: the vertex colour, mixed with a texel of the
// texture atlas (or four, mixed, with flag 256) as the polygon asks (modulated, as a decal, or with the toon
// table), written with its colour times its alpha; and, for fog, its depth
// (24 bits) and whether it takes fog (the pipelines keep the depth where the polygon keeps its depth, and
// the mark where every polygon drawn on the pixel takes fog); and the number
// of the opaque polygon drawn there, for outlines above the DS's resolution
// (only the pipelines of opaque pixels write it).
#version 450

layout(location = 0) in vec2 v_texel;
layout(location = 1) in vec4 v_colour;
layout(location = 2) flat in uvec4 v_info;
layout(location = 3) in float v_w_depth;

layout(location = 0) out vec4 o_colour;
layout(location = 1) out vec4 o_fog;
layout(location = 2) out vec4 o_number;  // r: the polygon number / 63; a: 1.

layout(set = 2, binding = 0) uniform sampler2D u_atlas;
layout(set = 3, binding = 0) uniform Scene {
  vec4 u_toon[32];
  // x: the alpha under which nothing is drawn; y: 1 for highlight; z: which
  // pixels are drawn: 0 all, as they are; 1 all, as opaque ones; 2 those of
  // less than full alpha; 3 those of full alpha, as opaque ones (the DS
  // draws those of a translucent polygon so); w: 1 where depths are w
  // (nds::HardwareScene::depth_is_w).
  vec4 u_options;
};

// A texel coordinate inside a texture `size` texels wide: repeated (every
// other time mirrored, if asked) or held at the edge.
int wrapped(int c, int size, bool repeat, bool flip) {
  if (!repeat) return clamp(c, 0, size - 1);
  if (flip && (c & size) != 0) c = -1 - c;
  return c & (size - 1);
}

// Texel `st` of the texture at `place` in the atlas, `size` texels, wrapped
// as `flags` say: never a neighbour's in the atlas.
vec4 atlas_texel(ivec2 st, ivec2 place, ivec2 size, uint flags) {
  st.x = wrapped(st.x, size.x, (flags & 8u) != 0u, (flags & 32u) != 0u);
  st.y = wrapped(st.y, size.y, (flags & 16u) != 0u, (flags & 64u) != 0u);
  return texelFetch(u_atlas, place + st, 0);
}

// Mixed from the four texels whose middles are around the point, as
// nds::PolygonRenderer::smoothed_texel() mixes them: with an alpha of their
// own (flag 512), by weight and alpha, so that what is not there does not
// darken what is; else drawn where at least half the weight is there, from
// what is.
vec4 smoothed_texel(vec2 at, ivec2 place, ivec2 size, uint flags) {
  vec2 st = at - 0.5;
  ivec2 first = ivec2(floor(st));
  vec2 f = st - vec2(first);
  vec4 texels[4] = vec4[4](atlas_texel(first, place, size, flags), atlas_texel(first + ivec2(1, 0), place, size, flags),
                           atlas_texel(first + ivec2(0, 1), place, size, flags),
                           atlas_texel(first + ivec2(1, 1), place, size, flags));
  float weights[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
  vec3 parts = vec3(0.0);
  float sum = 0.0;
  bool own_alpha = (flags & 512u) != 0u;
  for (int i = 0; i < 4; ++i) {
    float w = own_alpha ? weights[i] * texels[i].a : (texels[i].a > 0.0 ? weights[i] : 0.0);
    parts += w * texels[i].rgb;
    sum += w;
  }
  if (own_alpha) return sum > 0.0 ? vec4(parts / sum, sum) : vec4(0.0);
  return sum >= 0.5 ? vec4(parts / sum, 1.0) : vec4(0.0);
}

void main() {
  vec4 colour = v_colour;
  uint flags = v_info.z;
  uint mode = flags & 3u;
  vec3 toon = u_toon[clamp(int(v_colour.r * 63.0 + 0.5) / 2, 0, 31)].rgb;  // By the red part.
  bool highlight = u_options.y > 0.5;
  if ((flags & 4u) != 0u) {
    ivec2 size = ivec2(int(v_info.y & 0xFFFFu), int(v_info.y >> 16));
    ivec2 place = ivec2(int(v_info.x & 0xFFFFu), int(v_info.x >> 16));
    vec4 texel = (flags & 256u) != 0u ? smoothed_texel(v_texel, place, size, flags)
                                      : atlas_texel(ivec2(floor(v_texel)), place, size, flags);
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
  int which = int(u_options.z + 0.5);
  bool full = colour.a >= 1.0 - 0.5 / 63.0;
  if ((which == 2 && full) || (which == 3 && !full)) discard;
  if (which == 1 || which == 3) colour.a = 1.0;
  o_colour = vec4(colour.rgb * colour.a, colour.a);
  // Depth by z goes in proportion across the screen, as the graphics
  // hardware takes it; depth by w with perspective.
  float d = clamp(u_options.w > 0.5 ? v_w_depth : gl_FragCoord.z, 0.0, 1.0);
  gl_FragDepth = d;
  uint depth = uint(d * 16777215.0 + 0.5);
  o_number = vec4(float(v_info.w & 63u) / 63.0, 0.0, 0.0, 1.0);
  o_fog = vec4(vec3(uvec3(depth >> 16, (depth >> 8) & 255u, depth & 255u)) / 255.0, (flags & 128u) != 0u ? 1.0 : 0.0);
}
