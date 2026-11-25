// A screen's picture (app/hardware_3d.h), made at more pixels than the
// window shows it at, made smaller (Hardware3D::downscaled): each pixel of
// the result is the average of the part of the picture it covers, each
// pixel of the picture by how much of it is covered. Bilinear filtering
// takes four pixels only, however much smaller, and so leaves the stairs on
// the edges of a picture made at 8x and shown at 3x.
#version 450

layout(location = 0) in vec2 v_place;
layout(location = 0) out vec4 o_colour;

layout(set = 2, binding = 0) uniform sampler2D u_picture;
layout(set = 3, binding = 0) uniform Resolve {
  // x: the picture's first column taken; y: how many columns are taken (all
  // lines are); z and w: the result's width and height.
  vec4 u_from;
};

void main() {
  const vec2 size = vec2(textureSize(u_picture, 0));
  const vec2 step = vec2(u_from.y / u_from.z, size.y / u_from.w);
  const vec2 pixel = floor(gl_FragCoord.xy);
  const vec2 start = vec2(u_from.x, 0.0) + pixel * step, end = start + step;
  const ivec2 first = ivec2(floor(start)), last = min(ivec2(ceil(end)) - 1, ivec2(size) - 1);
  vec4 sum = vec4(0.0);
  float weight = 0.0;
  for (int y = max(first.y, 0); y <= last.y; ++y) {
    const float h = min(end.y, float(y + 1)) - max(start.y, float(y));
    for (int x = max(first.x, 0); x <= last.x; ++x) {
      const float w = (min(end.x, float(x + 1)) - max(start.x, float(x))) * h;
      sum += texelFetch(u_picture, ivec2(x, y), 0) * w;
      weight += w;
    }
  }
  o_colour = weight > 0.0 ? sum / weight : vec4(0.0, 0.0, 0.0, 1.0);
}
