// One triangle that covers the target, with texture coordinates from the
// top left (0, 0) to the bottom right (1, 1).
#version 450

layout(location = 0) out vec2 v_place;

void main() {
  vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_place = vec2(corner.x, 1.0 - corner.y);
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
