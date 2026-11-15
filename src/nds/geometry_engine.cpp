#include "nds/geometry_engine.h"

#include <algorithm>

namespace bd::nds {

namespace {

using Matrix = GeometryEngine::Matrix;
using i32 = std::int32_t;
using i64 = std::int64_t;
using u32 = std::uint32_t;

// Parameter words per command number; numbers above 0x7F take none.
constexpr std::uint8_t kParameters[0x80] = {
    0, 0, 0, 0, 0,  0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x00
    1, 0, 1, 1, 1,  0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0,  // 0x10
    1, 1, 1, 2, 1,  1, 1,  1,  1,  1,  1, 1, 0, 0, 0, 0,  // 0x20
    1, 1, 1, 1, 32, 0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x30
    1, 0, 0, 0, 0,  0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x40
    1, 0, 0, 0, 0,  0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x50
    1, 0, 0, 0, 0,  0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x60
    3, 2, 1, 0, 0,  0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  // 0x70
};

// a times b: each row of a through b. Products are 64 bits wide before the
// twelve fraction bits are dropped; the result keeps the low 32.
Matrix times(const Matrix& a, const Matrix& b) {
  Matrix r{};
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      i64 sum = 0;
      for (int k = 0; k < 4; ++k) sum += static_cast<i64>(a[y * 4 + k]) * b[k * 4 + x];
      r[y * 4 + x] = static_cast<i32>(sum >> 12);
    }
  }
  return r;
}

struct Point {
  i32 x = 0, y = 0, z = 0, w = 0;
};

Point through(const Point& p, const Matrix& m) {
  const auto row = [&](int column) {
    return static_cast<i32>((static_cast<i64>(p.x) * m[column] + static_cast<i64>(p.y) * m[4 + column] +
                             static_cast<i64>(p.z) * m[8 + column] + static_cast<i64>(p.w) * m[12 + column]) >>
                            12);
  };
  return Point{row(0), row(1), row(2), row(3)};
}

// Where the edge from `a` to `b` crosses one side of the view volume, given
// each end's distance to that side.
Point crossing(const Point& a, const Point& b, i32 value_a, i32 value_b) {
  const i64 d1 = static_cast<i64>(value_a) + a.w;
  const i64 d2 = static_cast<i64>(value_b) + b.w;
  if (d2 == d1) return a;
  const auto between = [&](i32 from, i32 to) { return static_cast<i32>((d2 * from - d1 * to) / (d2 - d1)); };
  return Point{between(a.x, b.x), between(a.y, b.y), between(a.z, b.z), between(a.w, b.w)};
}

// Whether any of a four-cornered face is left after cutting it down to the
// view volume (each of x, y, z between -w and w).
bool face_in_view(const Point* face) {
  Point now[10], next[10];
  int size = 4;
  std::copy(face, face + 4, now);
  for (int side = 0; side < 6; ++side) {
    int kept = 0;
    for (int j = 0; j < size; ++j) {
      const Point& current = now[j];
      const Point& previous = now[(j - 1 + size) % size];
      const auto value = [&](const Point& p) {
        const i32 v = side < 2 ? p.x : side < 4 ? p.y : p.z;
        // Negated in 32 bits, as the hardware's arithmetic wraps.
        return (side & 1) != 0 ? static_cast<i32>(0u - static_cast<u32>(v)) : v;
      };
      const i32 current_value = value(current), previous_value = value(previous);
      const bool current_in = static_cast<i64>(current_value) >= -static_cast<i64>(current.w);
      const bool previous_in = static_cast<i64>(previous_value) >= -static_cast<i64>(previous.w);
      if (current_in) {
        if (!previous_in) next[kept++] = crossing(current, previous, current_value, previous_value);
        next[kept++] = current;
      } else if (previous_in) {
        next[kept++] = crossing(current, previous, current_value, previous_value);
      }
    }
    size = kept;
    std::copy(next, next + size, now);
    if (size == 0) return false;
  }
  return size > 0;
}

// The low 16 bits of a word, and its high 16, as signed numbers.
std::int16_t low16(u32 word) { return static_cast<std::int16_t>(word & 0xFFFF); }
std::int16_t high16(u32 word) { return static_cast<std::int16_t>(word >> 16); }
// One of the three signed 10-bit fields of a word.
i32 ten_bits(u32 word, unsigned shift) {
  const u32 field = (word >> shift) & 0x3FF;
  return (field & 0x200) != 0 ? static_cast<i32>(field) - 0x400 : static_cast<i32>(field);
}

bool is_test(unsigned command) { return command >= GeometryEngine::kBoxTest && command <= GeometryEngine::kVectorTest; }
bool is_stack_command(unsigned command) {
  return command == GeometryEngine::kMatrixPush || command == GeometryEngine::kMatrixPop;
}

}  // namespace

unsigned GeometryEngine::parameters(unsigned command) { return command < 0x80 ? kParameters[command] : 0; }

bool GeometryEngine::exists(unsigned command) {
  return (command >= 0x10 && command <= 0x1C) || (command >= 0x20 && command <= 0x2B) ||
         (command >= 0x30 && command <= 0x34) || command == 0x40 || command == 0x41 || command == 0x50 ||
         command == 0x60 || (command >= 0x70 && command <= 0x72);
}

const char* GeometryEngine::command_name(unsigned command) {
  switch (command) {
    case 0x00: return "nothing (padding)";
    case kMatrixMode: return "choose the matrix";
    case kMatrixPush: return "push the matrix";
    case kMatrixPop: return "pop the matrix";
    case kMatrixStore: return "store the matrix";
    case kMatrixRestore: return "restore the matrix";
    case kMatrixIdentity: return "set the matrix to the identity";
    case kMatrixLoad4x4: return "load a 4 by 4 matrix";
    case kMatrixLoad4x3: return "load a 4 by 3 matrix";
    case kMatrixMultiply4x4: return "multiply by a 4 by 4 matrix";
    case kMatrixMultiply4x3: return "multiply by a 4 by 3 matrix";
    case kMatrixMultiply3x3: return "multiply by a 3 by 3 matrix";
    case kMatrixScale: return "scale";
    case kMatrixTranslate: return "translate";
    case kColour: return "vertex colour";
    case kNormal: return "normal";
    case kTextureCoordinates: return "texture coordinates";
    case kVertex16: return "vertex, 16 bits a coordinate";
    case kVertex10: return "vertex, 10 bits a coordinate";
    case kVertexXY: return "vertex, x and y";
    case kVertexXZ: return "vertex, x and z";
    case kVertexYZ: return "vertex, y and z";
    case kVertexDifference: return "vertex, as a difference";
    case kPolygonAttributes: return "polygon attributes";
    case kTextureParameters: return "texture parameters";
    case kPaletteBase: return "texture palette";
    case kDiffuseAmbient: return "material: diffuse and ambient";
    case kSpecularEmission: return "material: specular and emission";
    case kLightVector: return "light direction";
    case kLightColour: return "light colour";
    case kShininess: return "shininess table";
    case kBeginVertices: return "begin a vertex list";
    case kEndVertices: return "end a vertex list";
    case kSwapBuffers: return "swap buffers";
    case kViewport: return "viewport";
    case kBoxTest: return "is a box in view";
    case kPositionTest: return "a position through the matrices";
    case kVectorTest: return "a vector through the vector matrix";
    default: return "no command";
  }
}

GeometryEngine::GeometryEngine(Interrupts& interrupts) : interrupts_(interrupts) {
  position_stack_.fill(kIdentity);
  vector_stack_.fill(kIdentity);
}

void GeometryEngine::attach(Arm9Io& io) {
  for (u32 i = 0; i < 16; ++i)
    io.add(kFifoPort + 4 * i, 4, nullptr, [this](u32 value, u32 mask) { write_fifo(value & mask); });
  for (unsigned command = 0x10; command < 0x80; ++command) {
    if (!exists(command)) continue;
    io.add(kCommandPorts + 4 * command, 4, nullptr,
           [this, command](u32 value, u32 mask) { write_command(command, value & mask); });
  }
  io.add(kStatus, 4, [this] { return status(); }, [this](u32 value, u32 mask) { write_status(value, mask); });
  // Nothing is counted yet: see the header.
  io.add(kCounts, 4, [] { return u32{0}; }, nullptr);
  for (u32 i = 0; i < 4; ++i)
    io.add(kPositionResult + 4 * i, 4, [this, i] { return static_cast<u32>(position_result_[i]); }, nullptr);
  for (u32 i = 0; i < 3; ++i)
    io.add(kVectorResult + 2 * i, 2, [this, i] { return static_cast<u32>(vector_result_[i]) & 0xFFFFu; }, nullptr);
  for (u32 i = 0; i < 16; ++i)
    io.add(kClipMatrix + 4 * i, 4, [this, i] { return static_cast<u32>(clip_matrix()[i]); }, nullptr);
  for (u32 i = 0; i < 9; ++i)
    io.add(kVectorMatrix + 4 * i, 4, [this, i] { return static_cast<u32>(vector_[(i / 3) * 4 + i % 3]); }, nullptr);
}

Matrix GeometryEngine::clip_matrix() const { return times(position_, projection_); }

std::size_t GeometryEngine::queued() const { return queue_.size() > 4 ? queue_.size() - 4 : 0; }

std::uint32_t GeometryEngine::status() const {
  const std::size_t waiting = std::min(queued(), kQueueSize);
  u32 value = flags_;
  if (queued_tests_ != 0) value |= kTestBusy;
  value |= static_cast<u32>(position_level_) << kPositionLevelShift;
  if (projection_level_) value |= kProjectionLevel;
  if (queued_stack_commands_ != 0) value |= kStackBusy;
  value |= static_cast<u32>(waiting) << kQueuedShift;
  if (waiting < kQueueSize / 2) value |= kLessThanHalfFull;
  if (waiting == 0) value |= kEmpty;
  if (stopped_ || !queue_.empty()) value |= kBusy;
  return value;
}

void GeometryEngine::write_status(std::uint32_t value, std::uint32_t mask) {
  if ((value & mask & kStackError) != 0) {
    // Acknowledging the error also empties the projection stack.
    flags_ &= ~kStackError;
    projection_level_ = false;
  }
  mask &= 3u << kInterruptShift;
  flags_ = (flags_ & ~mask) | (value & mask);
  changed();
}

void GeometryEngine::changed() {
  statistics_.most_queued = std::max<std::uint64_t>(statistics_.most_queued, queued());
  const unsigned choice = flags_ >> kInterruptShift;
  interrupts_.set_level(Interrupts::kGeometryFifo,
                        (choice == 1 && less_than_half_full()) || (choice == 2 && queued() == 0));
  const bool half = less_than_half_full();
  const bool was = was_less_than_half_full_;
  was_less_than_half_full_ = half;
  if (half && !was && on_less_than_half_full) on_less_than_half_full();
}

void GeometryEngine::write_fifo(std::uint32_t word) {
  if (pack_ == 0) {
    pack_ = word;
  } else {
    add(Entry{static_cast<std::uint8_t>(pack_), word});
    if (++pack_received_ == parameters(pack_ & 0xFF)) {
      pack_ >>= 8;
      pack_received_ = 0;
    }
  }
  // Commands without parameters need no further word.
  while (pack_ != 0 && parameters(pack_ & 0xFF) == 0) {
    add(Entry{static_cast<std::uint8_t>(pack_), 0});
    pack_ >>= 8;
  }
}

void GeometryEngine::write_command(unsigned command, std::uint32_t parameter) {
  add(Entry{static_cast<std::uint8_t>(command), parameter});
}

void GeometryEngine::add(Entry entry) {
  if (!room()) {
    ++statistics_.waits_for_room;
    if (on_full) on_full();
  }
  queue_.push_back(entry);
  if (is_test(entry.command)) ++queued_tests_;
  if (is_stack_command(entry.command)) ++queued_stack_commands_;
  run();
  changed();
}

void GeometryEngine::vertical_blank() {
  if (stopped_) {
    stopped_ = false;
    ++statistics_.swaps;
    run();
  }
  changed();
}

void GeometryEngine::run() {
  while (!stopped_ && !queue_.empty()) {
    const unsigned command = queue_.front().command;
    const unsigned entries = std::max(1u, parameters(command));
    if (queue_.size() < entries) break;
    std::uint32_t p[32];
    for (unsigned i = 0; i < entries; ++i) p[i] = queue_[i].parameter;
    queue_.erase(queue_.begin(), queue_.begin() + entries);
    if (is_test(command)) queued_tests_ -= std::min(queued_tests_, entries);
    if (is_stack_command(command)) --queued_stack_commands_;
    ++statistics_.commands[command];
    execute(command, p);
  }
}

void GeometryEngine::load(const Matrix& m) {
  switch (mode_) {
    case 0: projection_ = m; break;
    case 1: position_ = m; break;
    case 2: position_ = vector_ = m; break;
    default: texture_ = m; break;
  }
}

void GeometryEngine::multiply(const Matrix& m, bool vector_too) {
  switch (mode_) {
    case 0: projection_ = times(m, projection_); break;
    case 1: position_ = times(m, position_); break;
    case 2:
      position_ = times(m, position_);
      if (vector_too) vector_ = times(m, vector_);
      break;
    default: texture_ = times(m, texture_); break;
  }
}

void GeometryEngine::push() {
  switch (mode_) {
    case 0:
      // One level.
      if (projection_level_) {
        flags_ |= kStackError;
      } else {
        projection_stack_ = projection_;
        projection_level_ = true;
      }
      break;
    case 1:
    case 2:
      // Position and vector share a stack of 31 levels. The last is there
      // but using it counts as an error.
      if (position_level_ >= 30) flags_ |= kStackError;
      if (position_level_ < 31) {
        position_stack_[position_level_] = position_;
        vector_stack_[position_level_] = vector_;
        ++position_level_;
      }
      break;
    default:
      texture_stack_ = texture_;
      break;
  }
}

void GeometryEngine::pop(std::uint32_t parameter) {
  switch (mode_) {
    case 0:
      if (projection_level_) {
        projection_level_ = false;
        projection_ = projection_stack_;
      } else {
        flags_ |= kStackError;
      }
      break;
    case 1:
    case 2: {
      // The parameter is how many levels to go back: six bits, signed.
      const int by = (parameter & 0x20) != 0 ? static_cast<int>(parameter & 0x3F) - 0x40 : static_cast<int>(parameter & 0x3F);
      const int level = static_cast<int>(position_level_) - by;
      if (level < 0 || level >= 30) flags_ |= kStackError;
      if (level >= 0 && level < 31) {
        position_level_ = static_cast<unsigned>(level);
        position_ = position_stack_[position_level_];
        vector_ = vector_stack_[position_level_];
      }
      break;
    }
    default:
      texture_ = texture_stack_;
      break;
  }
}

void GeometryEngine::store(std::uint32_t parameter) {
  switch (mode_) {
    case 0: projection_stack_ = projection_; break;
    case 1:
    case 2: {
      const unsigned at = parameter & 31;
      if (at == 31) flags_ |= kStackError;
      position_stack_[at] = position_;
      vector_stack_[at] = vector_;
      break;
    }
    default: texture_stack_ = texture_; break;
  }
}

void GeometryEngine::restore(std::uint32_t parameter) {
  switch (mode_) {
    case 0: projection_ = projection_stack_; break;
    case 1:
    case 2: {
      const unsigned at = parameter & 31;
      if (at == 31) flags_ |= kStackError;
      position_ = position_stack_[at];
      vector_ = vector_stack_[at];
      break;
    }
    default: texture_ = texture_stack_; break;
  }
}

void GeometryEngine::box_test(const std::uint32_t* p) {
  // A corner and three sizes, 16 bits each; the far corner wraps in 16 bits.
  std::int16_t c[6] = {low16(p[0]), high16(p[0]), low16(p[1]), high16(p[1]), low16(p[2]), high16(p[2])};
  for (int i = 0; i < 3; ++i) c[3 + i] = static_cast<std::int16_t>(c[3 + i] + c[i]);
  static constexpr std::uint8_t corners[8][3] = {{0, 1, 2}, {3, 1, 2}, {0, 4, 2}, {0, 1, 5},
                                                 {3, 4, 2}, {3, 1, 5}, {0, 4, 5}, {3, 4, 5}};
  const Matrix clip = clip_matrix();
  Point v[8];
  for (int i = 0; i < 8; ++i) v[i] = through(Point{c[corners[i][0]], c[corners[i][1]], c[corners[i][2]], kOne}, clip);
  const Point faces[6][4] = {{v[0], v[1], v[4], v[2]}, {v[3], v[5], v[7], v[6]}, {v[3], v[5], v[1], v[0]},
                             {v[6], v[7], v[4], v[2]}, {v[0], v[3], v[6], v[2]}, {v[1], v[5], v[7], v[4]}};
  flags_ &= ~kBoxInView;
  for (const auto& face : faces) {
    if (face_in_view(face)) {
      flags_ |= kBoxInView;
      break;
    }
  }
}

void GeometryEngine::execute(unsigned command, const std::uint32_t* p) {
  const auto word = [&](unsigned i) { return static_cast<i32>(p[i]); };
  switch (command) {
    case 0x00: break;  // The command that does nothing, for padding.
    case kMatrixMode: mode_ = p[0] & 3; break;
    case kMatrixPush: push(); break;
    case kMatrixPop: pop(p[0]); break;
    case kMatrixStore: store(p[0]); break;
    case kMatrixRestore: restore(p[0]); break;
    case kMatrixIdentity: load(kIdentity); break;
    case kMatrixLoad4x4:
    case kMatrixMultiply4x4: {
      Matrix m;
      for (unsigned i = 0; i < 16; ++i) m[i] = word(i);
      if (command == kMatrixLoad4x4) load(m); else multiply(m);
      break;
    }
    case kMatrixLoad4x3:
    case kMatrixMultiply4x3: {
      // Four rows of three; the fourth column is that of the identity.
      Matrix m = kIdentity;
      for (unsigned i = 0; i < 12; ++i) m[(i / 3) * 4 + i % 3] = word(i);
      if (command == kMatrixLoad4x3) load(m); else multiply(m);
      break;
    }
    case kMatrixMultiply3x3: {
      Matrix m = kIdentity;
      for (unsigned i = 0; i < 9; ++i) m[(i / 3) * 4 + i % 3] = word(i);
      multiply(m);
      break;
    }
    case kMatrixScale: {
      Matrix m = kIdentity;
      for (unsigned i = 0; i < 3; ++i) m[i * 5] = word(i);
      multiply(m, false);  // Scaling leaves the vector matrix alone.
      break;
    }
    case kMatrixTranslate: {
      Matrix m = kIdentity;
      for (unsigned i = 0; i < 3; ++i) m[12 + i] = word(i);
      multiply(m);
      break;
    }

    case kVertex16:
      vertex_ = {low16(p[0]), high16(p[0]), low16(p[1])};
      break;
    case kVertex10:
      // Ten bits each, in units 64 times as large.
      for (unsigned i = 0; i < 3; ++i) vertex_[i] = static_cast<std::int16_t>(ten_bits(p[0], 10 * i) * 64);
      break;
    case kVertexXY:
      vertex_[0] = low16(p[0]);
      vertex_[1] = high16(p[0]);
      break;
    case kVertexXZ:
      vertex_[0] = low16(p[0]);
      vertex_[2] = high16(p[0]);
      break;
    case kVertexYZ:
      vertex_[1] = low16(p[0]);
      vertex_[2] = high16(p[0]);
      break;
    case kVertexDifference:
      // Ten bits each, added to the position, wrapping in 16 bits.
      for (unsigned i = 0; i < 3; ++i) vertex_[i] = static_cast<std::int16_t>(vertex_[i] + ten_bits(p[0], 10 * i));
      break;

    case kSwapBuffers:
      swap_parameter_ = p[0];
      stopped_ = true;
      break;
    case kViewport:
      viewport_ = p[0];
      break;

    case kBoxTest:
      box_test(p);
      break;
    case kPositionTest: {
      // The position also becomes the current vertex position.
      vertex_ = {low16(p[0]), high16(p[0]), low16(p[1])};
      const Point r = through(Point{vertex_[0], vertex_[1], vertex_[2], kOne}, clip_matrix());
      position_result_ = {r.x, r.y, r.z, r.w};
      break;
    }
    case kVectorTest: {
      // Ten bits each, in units eight times as large; through the vector
      // matrix's 3 by 3; the answers keep 13 bits, sign included.
      const Point in{ten_bits(p[0], 0) * 8, ten_bits(p[0], 10) * 8, ten_bits(p[0], 20) * 8, 0};
      const Point r = through(in, vector_);
      const i32 out[3] = {r.x, r.y, r.z};
      for (unsigned i = 0; i < 3; ++i) {
        const u32 bits = static_cast<u32>(out[i]) & 0x1FFF;
        vector_result_[i] = static_cast<std::int16_t>((bits & 0x1000) != 0 ? static_cast<i32>(bits) - 0x2000 : static_cast<i32>(bits));
      }
      break;
    }

    default:
      // Colours, normals, texture coordinates, polygon and light settings,
      // and the begin and end of a vertex list: accepted, without effect yet.
      if (!exists(command)) ++statistics_.unknown_commands;
      break;
  }
}

}  // namespace bd::nds
