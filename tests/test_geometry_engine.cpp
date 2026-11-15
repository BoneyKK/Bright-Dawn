// The 3D hardware's command queue, matrices, tests and status register.
#include <cstdint>
#include <initializer_list>

#include "nds/arm9_io.h"
#include "nds/geometry_engine.h"
#include "nds/interrupts.h"
#include "test.h"

namespace {

using bd::nds::GeometryEngine;
using Matrix = GeometryEngine::Matrix;

constexpr std::uint32_t kIdle = GeometryEngine::kEmpty | GeometryEngine::kLessThanHalfFull;
constexpr std::int32_t kOne = GeometryEngine::kOne;

// A number of whole and half units in the hardware's fixed point.
constexpr std::uint32_t fx(int halves) { return static_cast<std::uint32_t>(halves * (kOne / 2)); }

struct Fixture {
  bd::nds::Interrupts irq;
  GeometryEngine g{irq};

  void send(unsigned command, std::initializer_list<std::uint32_t> parameters = {}) {
    if (parameters.size() == 0) g.write_command(command, 0);
    for (std::uint32_t p : parameters) g.write_command(command, p);
  }
  void mode(unsigned m) { send(GeometryEngine::kMatrixMode, {m}); }
  void translate(int x, int y, int z) { send(GeometryEngine::kMatrixTranslate, {fx(x), fx(y), fx(z)}); }
  void scale(int x, int y, int z) { send(GeometryEngine::kMatrixScale, {fx(x), fx(y), fx(z)}); }
  unsigned level() { return (g.status() >> GeometryEngine::kPositionLevelShift) & 31; }
  unsigned queued_in_status() { return (g.status() >> GeometryEngine::kQueuedShift) & 0x1FF; }
};

Matrix translation(int x, int y, int z) {
  Matrix m = GeometryEngine::kIdentity;
  m[12] = static_cast<std::int32_t>(fx(x));
  m[13] = static_cast<std::int32_t>(fx(y));
  m[14] = static_cast<std::int32_t>(fx(z));
  return m;
}

}  // namespace

TEST(geometry_starts_idle_with_identity_matrices) {
  Fixture f;
  CHECK(f.g.status() == kIdle);
  CHECK(f.g.projection() == GeometryEngine::kIdentity && f.g.position() == GeometryEngine::kIdentity);
  CHECK(f.g.vector() == GeometryEngine::kIdentity && f.g.texture() == GeometryEngine::kIdentity);
  CHECK(f.g.clip_matrix() == GeometryEngine::kIdentity);
  CHECK(f.g.queued() == 0 && f.g.room() && f.g.less_than_half_full() && !f.g.stopped_for_swap());
  CHECK(f.irq.requested() == 0);
}

TEST(geometry_command_parameter_counts) {
  CHECK(GeometryEngine::parameters(GeometryEngine::kMatrixMode) == 1);
  CHECK(GeometryEngine::parameters(GeometryEngine::kMatrixPush) == 0);
  CHECK(GeometryEngine::parameters(GeometryEngine::kMatrixLoad4x4) == 16);
  CHECK(GeometryEngine::parameters(GeometryEngine::kMatrixLoad4x3) == 12);
  CHECK(GeometryEngine::parameters(GeometryEngine::kMatrixMultiply3x3) == 9);
  CHECK(GeometryEngine::parameters(GeometryEngine::kVertex16) == 2);
  CHECK(GeometryEngine::parameters(GeometryEngine::kShininess) == 32);
  CHECK(GeometryEngine::parameters(GeometryEngine::kEndVertices) == 0);
  CHECK(GeometryEngine::parameters(GeometryEngine::kBoxTest) == 3);
  CHECK(GeometryEngine::parameters(GeometryEngine::kPositionTest) == 2);
  CHECK(GeometryEngine::parameters(0x00) == 0 && GeometryEngine::parameters(0x1D) == 0);
  CHECK(GeometryEngine::parameters(0xFF) == 0);
  CHECK(GeometryEngine::exists(0x1C) && !GeometryEngine::exists(0x1D) && !GeometryEngine::exists(0x00));
}

TEST(geometry_command_waits_for_its_last_parameter) {
  Fixture f;
  f.mode(1);
  for (std::uint32_t i = 0; i < 15; ++i) f.g.write_command(GeometryEngine::kMatrixLoad4x4, i + 1);
  CHECK(f.g.position() == GeometryEngine::kIdentity);
  // Fifteen entries wait; four of them the hardware has taken ahead.
  CHECK(f.g.queued() == 11 && f.queued_in_status() == 11);
  CHECK((f.g.status() & GeometryEngine::kEmpty) == 0);
  CHECK((f.g.status() & GeometryEngine::kLessThanHalfFull) != 0);
  CHECK((f.g.status() & GeometryEngine::kBusy) != 0);
  f.g.write_command(GeometryEngine::kMatrixLoad4x4, 16);
  for (int i = 0; i < 16; ++i) CHECK(f.g.position()[i] == i + 1);
  CHECK(f.g.vector() == GeometryEngine::kIdentity);  // Mode 1 is the position matrix alone.
  CHECK(f.g.status() == kIdle);
  CHECK(f.g.statistics().commands[GeometryEngine::kMatrixLoad4x4] == 1);
  // Up to four waiting entries leave the queue itself empty.
  f.g.write_command(GeometryEngine::kMatrixTranslate, 1);
  f.g.write_command(GeometryEngine::kMatrixTranslate, 2);
  CHECK(f.g.queued() == 0 && (f.g.status() & GeometryEngine::kEmpty) != 0);
  CHECK((f.g.status() & GeometryEngine::kBusy) != 0);
}

TEST(geometry_packed_commands) {
  Fixture f;
  // Four commands in one word, lowest byte first: matrix mode (one
  // parameter), identity (none), translate (three), scale (three).
  f.g.write_fifo(0x1B1C1510);
  CHECK(f.g.matrix_mode() == 0);
  f.g.write_fifo(1);  // The mode; identity follows by itself.
  CHECK(f.g.matrix_mode() == 1);
  CHECK(f.g.statistics().commands[GeometryEngine::kMatrixIdentity] == 1);
  f.g.write_fifo(fx(2));
  f.g.write_fifo(fx(4));
  f.g.write_fifo(fx(6));
  CHECK(f.g.position() == translation(2, 4, 6));
  f.g.write_fifo(fx(4));
  f.g.write_fifo(fx(4));
  CHECK(f.g.position() == translation(2, 4, 6));
  f.g.write_fifo(fx(4));
  // Scale times what was there: the scale on the diagonal, the translation
  // as it was.
  Matrix expected = translation(2, 4, 6);
  expected[0] = expected[5] = expected[10] = 2 * kOne;
  CHECK(f.g.position() == expected);
  CHECK(f.g.status() == kIdle);

  // The next word is a new set of commands. Without parameters they run at
  // once, and a zero byte among them does nothing.
  f.g.write_fifo(0x15001100);
  CHECK(f.level() == 1);
  CHECK(f.g.position() == GeometryEngine::kIdentity);
  CHECK(f.g.statistics().commands[0] == 2);
  // A word of zeros is no command at all.
  f.g.write_fifo(0);
  f.g.write_fifo(0);
  CHECK(f.g.statistics().commands[0] == 2);
  // Numbers that are no command are passed over.
  f.g.write_fifo(0x0000FF1D);
  CHECK(f.g.statistics().unknown_commands == 2);
  f.g.write_fifo(0x00000010);
  f.g.write_fifo(3);
  CHECK(f.g.matrix_mode() == 3);
}

TEST(geometry_multiplies_from_the_left) {
  // Translate, then scale: the scale applies to what comes out of the
  // translation's matrix.
  Fixture a;
  a.mode(1);
  a.translate(2, 4, 6);
  a.scale(4, 4, 4);
  Matrix expected = translation(2, 4, 6);
  expected[0] = expected[5] = expected[10] = 2 * kOne;
  CHECK(a.g.position() == expected);
  // Scale, then translate: the translation is scaled too.
  Fixture b;
  b.mode(1);
  b.scale(4, 4, 4);
  b.translate(2, 4, 6);
  expected = translation(4, 8, 12);
  expected[0] = expected[5] = expected[10] = 2 * kOne;
  CHECK(b.g.position() == expected);
  // The clip matrix is position times projection.
  b.mode(0);
  b.translate(0, 0, 2);
  expected[14] += kOne;
  CHECK(b.g.clip_matrix() == expected);
  CHECK(b.g.projection() == translation(0, 0, 2));
}

TEST(geometry_products_lose_their_low_bits_towards_minus_infinity) {
  Fixture f;
  f.mode(1);
  std::uint32_t m[16] = {};
  m[0] = 3;                                // 3/4096.
  m[5] = static_cast<std::uint32_t>(-3);   // -3/4096.
  m[10] = 0x7FFFFFFF;
  for (std::uint32_t v : m) f.g.write_command(GeometryEngine::kMatrixLoad4x4, v);
  f.scale(1, 1, 4);  // Halve x and y, double z.
  CHECK(f.g.position()[0] == 1);    // 1.5 -> 1.
  CHECK(f.g.position()[5] == -2);   // -1.5 -> -2.
  CHECK(f.g.position()[10] == -2);  // The product's low 32 bits.
}

TEST(geometry_matrix_modes_choose_the_matrices) {
  Fixture f;
  f.mode(0);
  f.translate(2, 0, 0);
  CHECK(f.g.projection() == translation(2, 0, 0) && f.g.position() == GeometryEngine::kIdentity);
  f.mode(3);
  f.translate(0, 2, 0);
  CHECK(f.g.texture() == translation(0, 2, 0) && f.g.position() == GeometryEngine::kIdentity);
  f.mode(1);
  f.translate(0, 0, 2);
  CHECK(f.g.position() == translation(0, 0, 2) && f.g.vector() == GeometryEngine::kIdentity);
  // Mode 2: position and vector together, except for scaling.
  f.mode(2);
  f.send(GeometryEngine::kMatrixIdentity);
  f.translate(2, 2, 2);
  CHECK(f.g.position() == translation(2, 2, 2) && f.g.vector() == translation(2, 2, 2));
  f.scale(4, 4, 4);
  CHECK(f.g.vector() == translation(2, 2, 2));
  CHECK(f.g.position()[0] == 2 * kOne && f.g.position()[12] == kOne);
  // A 3 by 3 and a 4 by 3 matrix are filled out with the identity's last
  // column (and last row).
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixMultiply3x3, {1, 2, 3, 4, 5, 6, 7, 8, 9});
  const Matrix three = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 0, 0, 0, kOne};
  CHECK(f.g.position() == three && f.g.vector() == three);
  f.send(GeometryEngine::kMatrixLoad4x3, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
  const Matrix four = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, kOne};
  CHECK(f.g.position() == four && f.g.vector() == four);
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixMultiply4x3, {fx(2), 0, 0, 0, fx(2), 0, 0, 0, fx(2), fx(6), 0, 0});
  CHECK(f.g.vector() == translation(6, 0, 0));
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixMultiply4x4, {fx(2), 0, 0, 0, 0, fx(2), 0, 0, 0, 0, fx(2), 0, 0, 0, 0, fx(4)});
  CHECK(f.g.position()[15] == 2 * kOne && f.g.position()[0] == kOne);
}

TEST(geometry_3_by_3_multiplication_fills_in_the_rest) {
  Fixture f;
  f.mode(1);
  f.send(GeometryEngine::kMatrixMultiply3x3, {fx(4), 0, 0, 0, fx(6), 0, 0, 0, fx(8)});
  const Matrix expected = {2 * kOne, 0, 0, 0, 0, 3 * kOne, 0, 0, 0, 0, 4 * kOne, 0, 0, 0, 0, kOne};
  CHECK(f.g.position() == expected);
}

TEST(geometry_position_stack) {
  Fixture f;
  f.mode(2);
  f.translate(2, 0, 0);
  f.send(GeometryEngine::kMatrixPush);
  CHECK(f.level() == 1);
  f.translate(0, 2, 0);
  f.send(GeometryEngine::kMatrixPush);
  CHECK(f.level() == 2);
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixPop, {1});
  CHECK(f.level() == 1);
  CHECK(f.g.position() == translation(2, 2, 0) && f.g.vector() == translation(2, 2, 0));
  // Going back by minus one goes forward: the level after, and what is there.
  f.send(GeometryEngine::kMatrixPop, {0x3F});
  CHECK(f.level() == 2);
  f.send(GeometryEngine::kMatrixPop, {2});
  CHECK(f.level() == 0 && f.g.position() == translation(2, 0, 0));
  CHECK((f.g.status() & GeometryEngine::kStackError) == 0);
  // Below the bottom: an error, and nothing moves.
  f.send(GeometryEngine::kMatrixPop, {1});
  CHECK((f.g.status() & GeometryEngine::kStackError) != 0);
  CHECK(f.level() == 0 && f.g.position() == translation(2, 0, 0));
  f.g.write_status(GeometryEngine::kStackError, 0xFFFFFFFF);
  CHECK((f.g.status() & GeometryEngine::kStackError) == 0);

  // Store and restore by place, without moving the level.
  f.send(GeometryEngine::kMatrixStore, {5});
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixRestore, {5});
  CHECK(f.g.position() == translation(2, 0, 0) && f.level() == 0);
  CHECK((f.g.status() & GeometryEngine::kStackError) == 0);
  f.send(GeometryEngine::kMatrixStore, {31});  // The last place works, and is an error.
  CHECK((f.g.status() & GeometryEngine::kStackError) != 0);
  f.g.write_status(GeometryEngine::kStackError, 0xFFFFFFFF);
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixRestore, {31});
  CHECK(f.g.position() == translation(2, 0, 0));
  CHECK((f.g.status() & GeometryEngine::kStackError) != 0);
  f.g.write_status(GeometryEngine::kStackError, 0xFFFFFFFF);

  // Thirty pushes are fine; the thirty-first works and is an error; after
  // that nothing more goes on.
  for (int i = 0; i < 30; ++i) f.send(GeometryEngine::kMatrixPush);
  CHECK(f.level() == 30 && (f.g.status() & GeometryEngine::kStackError) == 0);
  f.send(GeometryEngine::kMatrixPush);
  CHECK(f.level() == 31 && (f.g.status() & GeometryEngine::kStackError) != 0);
  f.send(GeometryEngine::kMatrixPush);
  CHECK(f.level() == 31);
}

TEST(geometry_projection_and_texture_stacks_have_one_level) {
  Fixture f;
  f.mode(0);
  f.translate(2, 0, 0);
  f.send(GeometryEngine::kMatrixPush);
  CHECK((f.g.status() & GeometryEngine::kProjectionLevel) != 0);
  f.translate(2, 0, 0);
  f.send(GeometryEngine::kMatrixPush);  // No second level.
  CHECK((f.g.status() & GeometryEngine::kStackError) != 0);
  f.send(GeometryEngine::kMatrixPop, {1});
  CHECK(f.g.projection() == translation(2, 0, 0));
  CHECK((f.g.status() & GeometryEngine::kProjectionLevel) == 0);
  f.g.write_status(GeometryEngine::kStackError, 0xFFFFFFFF);
  f.send(GeometryEngine::kMatrixPop, {1});  // Nothing to pop.
  CHECK((f.g.status() & GeometryEngine::kStackError) != 0);
  // Acknowledging the error empties the projection stack.
  f.send(GeometryEngine::kMatrixPush);
  CHECK((f.g.status() & GeometryEngine::kProjectionLevel) != 0);
  f.g.write_status(GeometryEngine::kStackError, 0xFFFFFFFF);
  CHECK((f.g.status() & (GeometryEngine::kProjectionLevel | GeometryEngine::kStackError)) == 0);
  f.send(GeometryEngine::kMatrixStore, {0});
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixRestore, {0});
  CHECK(f.g.projection() == translation(2, 0, 0));

  f.mode(3);
  f.translate(0, 4, 0);
  f.send(GeometryEngine::kMatrixPush);
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kMatrixPop, {1});
  CHECK(f.g.texture() == translation(0, 4, 0));
  CHECK(f.level() == 0 && (f.g.status() & GeometryEngine::kStackError) == 0);
}

TEST(geometry_position_test) {
  Fixture f;
  f.mode(1);
  f.translate(2, 4, 6);
  f.mode(0);
  // A projection that puts z into w as well.
  f.send(GeometryEngine::kMatrixLoad4x4, {fx(2), 0, 0, 0, 0, fx(2), 0, 0, 0, 0, fx(2), fx(2), 0, 0, 0, fx(2)});
  // The point (1, 0, -1).
  f.send(GeometryEngine::kPositionTest, {fx(2), static_cast<std::uint32_t>(-kOne) & 0xFFFF});
  // Through the position matrix: (2, 2, 2, 1). Through the projection: the
  // same, with w = z + w.
  bd::nds::Arm9Io io;
  f.g.attach(io);
  CHECK(io.read(GeometryEngine::kPositionResult + 0, 4) == fx(4));
  CHECK(io.read(GeometryEngine::kPositionResult + 4, 4) == fx(4));
  CHECK(io.read(GeometryEngine::kPositionResult + 8, 4) == fx(4));
  CHECK(io.read(GeometryEngine::kPositionResult + 12, 4) == fx(6));
  // The point is now the current vertex position.
  CHECK(f.g.vertex()[0] == kOne && f.g.vertex()[1] == 0 && f.g.vertex()[2] == -kOne);
  CHECK((f.g.status() & GeometryEngine::kTestBusy) == 0);
  // The matrices read back: position times projection, row by row.
  CHECK(io.read(GeometryEngine::kClipMatrix + 4 * 0, 4) == fx(2));
  CHECK(io.read(GeometryEngine::kClipMatrix + 4 * 11, 4) == fx(2));
  CHECK(io.read(GeometryEngine::kClipMatrix + 4 * 12, 4) == fx(2));
  CHECK(io.read(GeometryEngine::kClipMatrix + 4 * 14, 4) == fx(6));
  CHECK(io.read(GeometryEngine::kClipMatrix + 4 * 15, 4) == fx(8));
}

TEST(geometry_vector_test) {
  Fixture f;
  f.mode(2);
  // A quarter turn: x goes to y, y to minus x.
  const std::uint32_t minus_one = static_cast<std::uint32_t>(-kOne);
  f.send(GeometryEngine::kMatrixLoad4x3, {0, fx(2), 0, minus_one, 0, 0, 0, 0, fx(2), 0, 0, 0});
  // x = 511/512, y = -1, z = 0, ten bits each.
  f.send(GeometryEngine::kVectorTest, {0x1FFu | (0x200u << 10)});
  bd::nds::Arm9Io io;
  f.g.attach(io);
  // -1 times -1 is 1, which does not fit the answer's 13 bits and comes out
  // as -1.
  CHECK(io.read(GeometryEngine::kVectorResult + 0, 2) == 0xF000);
  CHECK(io.read(GeometryEngine::kVectorResult + 2, 2) == 0x0FF8);
  CHECK(io.read(GeometryEngine::kVectorResult + 4, 2) == 0);
  // The vector matrix reads back as its 3 by 3.
  CHECK(io.read(GeometryEngine::kVectorMatrix + 4 * 1, 4) == fx(2));
  CHECK(io.read(GeometryEngine::kVectorMatrix + 4 * 3, 4) == minus_one);
  CHECK(io.read(GeometryEngine::kVectorMatrix + 4 * 8, 4) == fx(2));
  CHECK(io.read(GeometryEngine::kVectorMatrix + 4 * 0, 4) == 0);

  // A plain case: half of x through the identity.
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kVectorTest, {0x100u});
  CHECK(io.read(GeometryEngine::kVectorResult + 0, 2) == 0x0800);
  CHECK(io.read(GeometryEngine::kVectorResult + 2, 2) == 0);
}

TEST(geometry_box_test) {
  // With identity matrices the view is the cube from -1 to 1.
  const auto box = [](int x, int y, int z, int w, int h, int d) {
    Fixture f;
    const auto half = [](int halves) { return fx(halves) & 0xFFFF; };
    f.send(GeometryEngine::kBoxTest, {half(x) | (half(y) << 16), half(z) | (half(w) << 16), half(h) | (half(d) << 16)});
    CHECK((f.g.status() & GeometryEngine::kTestBusy) == 0);
    return (f.g.status() & GeometryEngine::kBoxInView) != 0;
  };
  CHECK(box(-1, -1, -1, 2, 2, 2));  // Inside.
  CHECK(box(1, 1, 1, 2, 2, 2));     // Partly inside.
  CHECK(!box(4, 4, 4, 1, 1, 1));    // Outside.
  CHECK(!box(-6, -1, -1, 2, 2, 2)); // Outside, to one side.
  CHECK(box(-6, -1, -1, 9, 2, 2));  // Reaching in from that side.
  // Only the box's faces count: a box all around the view is "not in view".
  CHECK(!box(-8, -8, -8, 15, 15, 15));

  // The result stays until the next test.
  Fixture f;
  f.send(GeometryEngine::kBoxTest, {0xF800F800, 0x1000F800, 0x10001000});
  CHECK((f.g.status() & GeometryEngine::kBoxInView) != 0);
  f.mode(1);
  f.translate(20, 0, 0);
  CHECK((f.g.status() & GeometryEngine::kBoxInView) != 0);
  f.send(GeometryEngine::kBoxTest, {0xF800F800, 0x1000F800, 0x10001000});  // The same box, moved away.
  CHECK((f.g.status() & GeometryEngine::kBoxInView) == 0);
}

TEST(geometry_vertex_commands_move_the_current_position) {
  Fixture f;
  f.send(GeometryEngine::kVertex16, {0x00020001, 0xFFFF0003});
  CHECK(f.g.vertex()[0] == 1 && f.g.vertex()[1] == 2 && f.g.vertex()[2] == 3);
  f.send(GeometryEngine::kVertexXY, {0xFFFE0010});
  CHECK(f.g.vertex()[0] == 16 && f.g.vertex()[1] == -2 && f.g.vertex()[2] == 3);
  f.send(GeometryEngine::kVertexXZ, {0x00070020});
  CHECK(f.g.vertex()[0] == 32 && f.g.vertex()[1] == -2 && f.g.vertex()[2] == 7);
  f.send(GeometryEngine::kVertexYZ, {0x00090008});
  CHECK(f.g.vertex()[0] == 32 && f.g.vertex()[1] == 8 && f.g.vertex()[2] == 9);
  // Ten bits each, in steps of 64: 1, -1 and -512.
  f.send(GeometryEngine::kVertex10, {0x001u | (0x3FFu << 10) | (0x200u << 20)});
  CHECK(f.g.vertex()[0] == 64 && f.g.vertex()[1] == -64 && f.g.vertex()[2] == -32768);
  // Differences, ten bits each: +5, -1, +511, the last wrapping in 16 bits.
  f.send(GeometryEngine::kVertexDifference, {0x005u | (0x3FFu << 10) | (0x1FFu << 20)});
  CHECK(f.g.vertex()[0] == 69 && f.g.vertex()[1] == -65 && f.g.vertex()[2] == -32257);
  f.send(GeometryEngine::kVertexDifference, {0x200u << 20});
  CHECK(f.g.vertex()[2] == 32767);
  // The commands that describe polygons are taken and change nothing yet.
  f.send(GeometryEngine::kBeginVertices, {1});
  f.send(GeometryEngine::kColour, {0x7FFF});
  f.send(GeometryEngine::kEndVertices);
  f.send(GeometryEngine::kViewport, {0xBFFF0000});
  CHECK(f.g.viewport() == 0xBFFF0000);
  CHECK(f.g.statistics().unknown_commands == 0);
  CHECK(f.g.status() == kIdle);
}

TEST(geometry_stops_at_a_swap_until_the_vertical_blank) {
  Fixture f;
  f.mode(1);
  f.send(GeometryEngine::kSwapBuffers, {3});
  CHECK(f.g.stopped_for_swap() && f.g.swap_parameter() == 3);
  CHECK((f.g.status() & GeometryEngine::kBusy) != 0);
  CHECK((f.g.status() & GeometryEngine::kEmpty) != 0);
  // What comes now waits.
  f.translate(2, 0, 0);
  f.send(GeometryEngine::kMatrixPush);
  f.send(GeometryEngine::kPositionTest, {0, 0});
  CHECK(f.g.position() == GeometryEngine::kIdentity && f.level() == 0);
  CHECK(f.g.queued() == 2 && f.queued_in_status() == 2);
  CHECK((f.g.status() & GeometryEngine::kStackBusy) != 0);
  CHECK((f.g.status() & GeometryEngine::kTestBusy) != 0);
  CHECK((f.g.status() & GeometryEngine::kEmpty) == 0);
  CHECK(f.g.statistics().swaps == 0);
  f.g.vertical_blank();
  CHECK(!f.g.stopped_for_swap() && f.g.statistics().swaps == 1);
  CHECK(f.g.position() == translation(2, 0, 0) && f.level() == 1);
  CHECK(f.g.status() == (kIdle | (1u << GeometryEngine::kPositionLevelShift)));
  // A vertical blank with no swap asked for changes nothing.
  f.g.vertical_blank();
  CHECK(f.g.statistics().swaps == 1);
  // A second swap among the waiting commands stops the engine again.
  f.send(GeometryEngine::kSwapBuffers, {0});
  f.send(GeometryEngine::kMatrixIdentity);
  f.send(GeometryEngine::kSwapBuffers, {0});
  f.translate(0, 2, 0);
  f.g.vertical_blank();
  CHECK(f.g.stopped_for_swap() && f.g.position() == GeometryEngine::kIdentity);
  f.g.vertical_blank();
  CHECK(!f.g.stopped_for_swap() && f.g.position() == translation(0, 2, 0));
  CHECK(f.g.statistics().swaps == 3);
}

TEST(geometry_queue_fills_while_the_engine_is_stopped) {
  Fixture f;
  int full = 0, half = 0;
  f.g.on_full = [&] { ++full; };
  f.g.on_less_than_half_full = [&] { ++half; };
  f.send(GeometryEngine::kSwapBuffers, {0});
  for (int i = 0; i < 131; ++i) f.mode(1);
  CHECK(f.g.queued() == 127 && f.g.less_than_half_full());
  f.mode(1);
  CHECK(f.g.queued() == 128 && !f.g.less_than_half_full());
  CHECK((f.g.status() & GeometryEngine::kLessThanHalfFull) == 0);
  for (int i = 0; i < 128; ++i) f.mode(1);
  CHECK(f.g.queued() == 256 && !f.g.room() && full == 0);
  CHECK(f.queued_in_status() == 256);
  // The next entry finds the queue full. Nobody makes room here, so it is
  // queued all the same.
  f.mode(2);
  CHECK(full == 1 && f.g.statistics().waits_for_room == 1);
  CHECK(f.queued_in_status() == 256);
  CHECK(f.g.statistics().most_queued == 257);
  CHECK(half == 0);
  f.g.vertical_blank();
  CHECK(f.g.matrix_mode() == 2 && f.g.status() == kIdle);
  CHECK(half == 1);

  // With someone making room, the entry goes in after the wait.
  Fixture g;
  g.g.on_full = [&] { g.g.vertical_blank(); };
  g.send(GeometryEngine::kSwapBuffers, {0});
  for (int i = 0; i < 260; ++i) g.mode(1);
  g.mode(3);
  CHECK(g.g.statistics().waits_for_room == 1 && g.g.statistics().swaps == 1);
  CHECK(g.g.matrix_mode() == 3 && g.g.status() == kIdle);
}

TEST(geometry_interrupt_is_a_level) {
  Fixture f;
  bd::nds::Arm9Io io;
  f.irq.attach(io);
  f.g.attach(io);
  const std::uint32_t bit = 1u << bd::nds::Interrupts::kGeometryFifo;
  // Asked for "queue empty" while it is empty: the request stands, and
  // acknowledging it does not make it go away.
  io.write(GeometryEngine::kStatus + 3, 1, 0x80);
  CHECK(f.g.status() == (kIdle | 0x80000000));
  CHECK((f.irq.requested() & bit) != 0);
  io.write(0x04000214, 4, bit);
  CHECK((f.irq.requested() & bit) != 0);
  // No longer asked for: gone.
  io.write(GeometryEngine::kStatus + 3, 1, 0x00);
  CHECK((f.irq.requested() & bit) == 0);

  // "Less than half full" follows the queue as it fills and drains.
  io.write(GeometryEngine::kStatus, 4, 0x40000000);
  CHECK((f.irq.requested() & bit) != 0);
  io.write(0x04000540, 4, 0);  // Swap: the engine stops.
  for (int i = 0; i < 131; ++i) io.write(0x04000440, 4, 1);
  CHECK((f.irq.requested() & bit) != 0);
  io.write(0x04000440, 4, 1);
  CHECK((f.irq.requested() & bit) == 0);
  // "Empty" is not the case either while entries wait.
  io.write(GeometryEngine::kStatus, 4, 0x80000000);
  CHECK((f.irq.requested() & bit) == 0);
  f.g.vertical_blank();
  CHECK((f.irq.requested() & bit) != 0);
  // Other requests are acknowledged as ever.
  f.irq.request(bd::nds::Interrupts::kVBlank);
  io.write(0x04000214, 4, 0xFFFFFFFF);
  CHECK(f.irq.requested() == bit);
}

TEST(geometry_registers) {
  Fixture f;
  bd::nds::Arm9Io io;
  f.g.attach(io);
  // Each command's own port, and the sixteen words that are the queue's port.
  io.write(0x04000440, 4, 2);
  CHECK(f.g.matrix_mode() == 2);
  io.write(0x0400043C, 4, 0x00000010);
  io.write(0x04000404, 4, 1);
  CHECK(f.g.matrix_mode() == 1);
  io.write(0x04000470, 4, fx(2));
  io.write(0x04000470, 4, fx(4));
  io.write(0x04000470, 4, fx(6));
  CHECK(f.g.position() == translation(2, 4, 6));
  io.write(0x04000444, 4, 0);  // Push.
  CHECK(io.read(GeometryEngine::kStatus, 4) == (kIdle | 0x100));
  CHECK(io.read(GeometryEngine::kStatus + 1, 1) == 0x01);
  // The count of polygons and vertices is not kept yet.
  CHECK(io.read(GeometryEngine::kCounts, 4) == 0);
  // The ports cannot be read.
  CHECK(io.read(0x04000440, 4) == 0 && io.read(0x04000400, 4) == 0);
  CHECK(io.unmodelled().empty());
  // A port between commands belongs to nobody.
  io.write(0x04000474, 4, 1);
  CHECK(io.unmodelled().count(0x04000474) == 1);
  CHECK(f.g.statistics().unknown_commands == 0);
}
