// The 3D engine's lighting: the colour a vertex gets when a normal is sent.
// The expectations are worked out by hand from the hardware's arithmetic.
#include <cstdint>
#include <initializer_list>

#include "nds/geometry_engine.h"
#include "nds/interrupts.h"
#include "test.h"

namespace {

using bd::nds::GeometryEngine;

constexpr int W = GeometryEngine::kOne;
constexpr std::uint32_t kBoth = GeometryEngine::kDrawFront | GeometryEngine::kDrawBack;
constexpr std::uint32_t kWhite = 0x7FFF;

// Three signed numbers of ten bits in a word.
constexpr std::uint32_t ten(int x, int y, int z) {
  return (static_cast<std::uint32_t>(x) & 0x3FF) | (static_cast<std::uint32_t>(y) & 0x3FF) << 10 |
         (static_cast<std::uint32_t>(z) & 0x3FF) << 20;
}
// The same six bits in red, green and blue.
constexpr std::uint32_t grey(std::uint32_t six) { return six | six << 6 | six << 12; }

struct Fixture {
  bd::nds::Interrupts irq;
  GeometryEngine g{irq};

  void send(unsigned command, std::initializer_list<std::uint32_t> parameters = {}) {
    if (parameters.size() == 0) g.write_command(command, 0);
    for (std::uint32_t p : parameters) g.write_command(command, p);
  }
  // A vertex list of triangles with these lights turned on.
  void begin(std::uint32_t lights) {
    send(GeometryEngine::kPolygonAttributes, {kBoth | lights});
    send(GeometryEngine::kBeginVertices, {GeometryEngine::kTriangles});
  }
  void light(unsigned number, int x, int y, int z, std::uint32_t colour) {
    send(GeometryEngine::kLightVector, {ten(x, y, z) | number << 30});
    send(GeometryEngine::kLightColour, {colour | number << 30});
  }
  void material(std::uint32_t diffuse, std::uint32_t ambient, std::uint32_t specular, std::uint32_t emission,
                bool table = false) {
    send(GeometryEngine::kDiffuseAmbient, {diffuse | ambient << 16});
    send(GeometryEngine::kSpecularEmission, {specular | emission << 16 | (table ? 0x8000u : 0u)});
  }
  void normal(int x, int y, int z) { send(GeometryEngine::kNormal, {ten(x, y, z)}); }
  // The colour the next vertices get: a triangle is sent, and its last
  // vertex looked at.
  std::uint32_t colour() {
    send(GeometryEngine::kVertex10, {ten(0, 0, 0)});
    send(GeometryEngine::kVertex10, {ten(32, 0, 0)});
    send(GeometryEngine::kVertex10, {ten(0, 32, 0)});
    return g.building().vertices.empty() ? 0xFFFFFFFF : g.building().vertices.back().colour;
  }
  // The colour after this normal.
  std::uint32_t lit(int x, int y, int z) {
    normal(x, y, z);
    return colour();
  }
};

}  // namespace

TEST(lighting_diffuse_goes_with_the_angle_to_the_light) {
  Fixture f;
  // A white light shining straight into the screen on a white material.
  f.light(0, 0, 0, -512, kWhite);
  f.material(kWhite, 0, 0, 0);
  f.begin(1);
  // A normal straight at the viewer is 511/512 long: 63 * 63 * 4088 / 2^18.
  CHECK(f.lit(0, 0, 511) == grey(61));
  // Half as long, half as bright.
  CHECK(f.lit(0, 0, 256) == grey(31));
  // Sideways and away from the light: dark.
  CHECK(f.lit(511, 0, 0) == 0);
  CHECK(f.lit(0, 0, -512) == 0);
  // The light's colour and the material's are multiplied part by part.
  f.light(0, 0, 0, -512, 0x001F);
  CHECK(f.lit(0, 0, 511) == 61);
  f.light(0, 0, 0, -512, kWhite);
  f.material(0x03E0, 0, 0, 0);
  CHECK(f.lit(0, 0, 511) == 61u << 6);
}

TEST(lighting_ambient_and_emission_do_not_ask_for_the_angle) {
  Fixture f;
  // A red light; the material throws back all ambient light and gives off
  // some green by itself.
  f.light(0, 0, 0, -512, 0x001F);
  f.material(0, kWhite, 0, 10u << 5);
  f.begin(1);
  CHECK(f.lit(0, 0, 511) == (62u | 21u << 6));
  CHECK(f.lit(0, 0, -512) == (62u | 21u << 6));
  // With no light turned on, what it gives off is all.
  f.begin(0);
  CHECK(f.lit(0, 0, 511) == 21u << 6);
}

TEST(lighting_specular_and_its_table) {
  Fixture f;
  f.light(0, 0, 0, -512, kWhite);
  f.material(0, 0, kWhite, 0);
  f.begin(1);
  // The level is squared: 4088 * 4088 / 4096 is 4080, and 63 * 63 * 4080 /
  // 2^18 is 61; half the length gives a quarter of the level, 1024, and 15.
  CHECK(f.lit(0, 0, 511) == grey(61));
  CHECK(f.lit(0, 0, 256) == grey(15));
  // The table, when on, replaces the level: its entry for a 32nd of it (127
  // and 32 here), times 16.
  for (unsigned word = 0; word < 32; ++word)
    f.send(GeometryEngine::kShininess, {word == 31 ? 0x80000000u : word == 8 ? 0x000000FFu : 0u});
  CHECK(f.lit(0, 0, 511) == grey(61));
  f.material(0, 0, kWhite, 0, true);
  CHECK(f.lit(0, 0, 511) == grey(31));
  CHECK(f.lit(0, 0, 256) == grey(61));
  CHECK(f.lit(0, 0, 128) == 0);
}

TEST(lighting_adds_up_the_lights_the_polygon_attributes_turn_on) {
  Fixture f;
  f.light(0, 0, 0, -512, kWhite);
  f.light(1, 0, 0, -512, kWhite);
  f.material(kWhite, 0, 0, 0);
  f.begin(3);
  // 61 and 61, and no part goes above 63.
  CHECK(f.lit(0, 0, 511) == grey(63));
  f.begin(2);
  CHECK(f.lit(0, 0, 511) == grey(61));
  // A light that was never set up is dark.
  f.begin(4);
  CHECK(f.lit(0, 0, 511) == 0);
  // The attributes count from the next vertex list.
  f.send(GeometryEngine::kPolygonAttributes, {kBoth | 3});
  CHECK(f.lit(0, 0, 511) == 0);
  f.send(GeometryEngine::kBeginVertices, {GeometryEngine::kTriangles});
  CHECK(f.lit(0, 0, 511) == grey(63));
}

TEST(lighting_directions_and_normals_go_through_the_vector_matrix) {
  Fixture f;
  f.material(kWhite, 0, 0, 0);
  f.begin(1);
  // A matrix that turns z round.
  const auto turn_z = [&] {
    f.send(GeometryEngine::kMatrixMode, {2});
    f.send(GeometryEngine::kMatrixIdentity);
    for (std::int32_t value : {W, 0, 0, 0, W, 0, 0, 0, -W})
      f.send(GeometryEngine::kMatrixMultiply3x3, {static_cast<std::uint32_t>(value)});
  };
  // A light sent as shining out of the screen shines into it: the matrix
  // counts as it is when the direction is sent.
  turn_z();
  f.light(0, 0, 0, 511, kWhite);
  f.send(GeometryEngine::kMatrixIdentity);
  // -4088 * 4088 / 4096, rounded down, is -4081: 63 * 63 * 4081 / 2^18.
  CHECK(f.lit(0, 0, 511) == grey(61));
  CHECK(f.lit(0, 0, -512) == 0);
  // And a normal sent while the matrix turns z round is turned.
  turn_z();
  CHECK(f.lit(0, 0, -512) == grey(61));
  CHECK(f.lit(0, 0, 511) == 0);
}

TEST(lighting_the_last_of_colour_and_normal_counts) {
  Fixture f;
  f.light(0, 0, 0, -512, kWhite);
  f.material(kWhite, 0, 0, 0);
  f.begin(1);
  f.normal(0, 0, 511);
  f.send(GeometryEngine::kColour, {0x001F});
  CHECK(f.colour() == 63);
  CHECK(f.lit(0, 0, 511) == grey(61));
  // Setting the material's colours does not light anything anew, except
  // that the diffuse colour can be made the vertex colour.
  f.material(0x7C00, 0, 0, 0);
  CHECK(f.colour() == grey(61));
  f.send(GeometryEngine::kDiffuseAmbient, {0x8000u | 0x7C00u});
  CHECK(f.colour() == 63u << 12);
}

TEST(lighting_texture_coordinates_from_the_normal) {
  Fixture f;
  // A texture matrix that gives 16 for the normal's first part and -16 for
  // its second.
  f.send(GeometryEngine::kMatrixMode, {3});
  for (int i = 0; i < 16; ++i)
    f.send(GeometryEngine::kMatrixLoad4x4, {static_cast<std::uint32_t>(i == 0 ? 16 * W : i == 5 ? -16 * W : 0)});
  f.send(GeometryEngine::kTextureParameters, {2u << 30});
  f.begin(0);
  f.send(GeometryEngine::kTextureCoordinates, {100});
  // Until a normal comes, the coordinates are as sent.
  f.colour();
  REQUIRE(f.g.building().vertices.size() == 3);
  CHECK(f.g.building().vertices[2].s == 100 && f.g.building().vertices[2].t == 0);
  // 4088 * 16 / 4096 rounded down, added to 100; and 2048 * -16 / 4096.
  f.normal(511, 256, 0);
  f.colour();
  REQUIRE(f.g.building().vertices.size() == 6);
  CHECK(f.g.building().vertices[5].s == 115 && f.g.building().vertices[5].t == -8);
  // In another mode the normal leaves them alone.
  f.send(GeometryEngine::kTextureParameters, {0});
  f.send(GeometryEngine::kTextureCoordinates, {7});
  f.normal(511, 256, 0);
  f.colour();
  CHECK(f.g.building().vertices[8].s == 7 && f.g.building().vertices[8].t == 0);
}
