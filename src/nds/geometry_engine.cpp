#include "nds/geometry_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "debug/frame_log.h"

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

using Vertex = GeometryEngine::Vertex;

// The place of a vertex after a matrix: the row (x, y, z, w) times `m`.
void through(Vertex& v, const Matrix& m) {
  const auto column = [&](int c) {
    return static_cast<i32>((static_cast<i64>(v.x) * m[c] + static_cast<i64>(v.y) * m[4 + c] +
                             static_cast<i64>(v.z) * m[8 + c] + static_cast<i64>(v.w) * m[12 + c]) >>
                            12);
  };
  const i32 x = column(0), y = column(1), z = column(2), w = column(3);
  v.x = x, v.y = y, v.z = z, v.w = w;
}

Vertex at(i32 x, i32 y, i32 z, i32 w) {
  Vertex v;
  v.x = x, v.y = y, v.z = z, v.w = w;
  return v;
}

// 64-bit arithmetic that wraps, as the hardware's does, rather than being
// undefined when a result does not fit.
i64 wrapping_times(i64 a, i64 b) { return static_cast<i64>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b)); }
i64 wrapping_plus(i64 a, i64 b) { return static_cast<i64>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b)); }
i64 wrapping_minus(i64 a, i64 b) { return static_cast<i64>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)); }

// Where the edge from `a` to `b` crosses one side of the view volume, given
// each end's distance to that side (d1 and d2; 0 or more inside). Everything
// a vertex carries is taken in proportion.
Vertex crossing(const Vertex& a, const Vertex& b, i64 d1, i64 d2) {
  if (d2 == d1) return a;
  const auto between = [&](i64 from, i64 to) { return (d2 * from - d1 * to) / (d2 - d1); };
  Vertex v;
  v.x = static_cast<i32>(between(a.x, b.x));
  v.y = static_cast<i32>(between(a.y, b.y));
  v.z = static_cast<i32>(between(a.z, b.z));
  v.w = static_cast<i32>(between(a.w, b.w));
  v.s = static_cast<std::int16_t>(between(a.s, b.s));
  v.t = static_cast<std::int16_t>(between(a.t, b.t));
  v.colour = 0;
  for (unsigned shift = 0; shift < 18; shift += 6) {
    const i64 part = between((a.colour >> shift) & 0x3F, (b.colour >> shift) & 0x3F);
    v.colour |= (static_cast<u32>(part) & 0x3F) << shift;
  }
  return v;
}

// A polygon without perspective, cut to the view, that reaches both sides of
// the screen (x = -w and w): the corners on them moved on to the sides of a
// picture a third wider (x = -4/3 w and 4/3 w), with their depth, colours
// and texture coordinates carried on as they change across the polygon (a
// texture goes on, not stretched).
void widen_across(Vertex* v, std::size_t size) {
  const auto on_side = [&](const Vertex& p, int side) {
    const i64 tolerance = std::max<i64>(std::abs(static_cast<i64>(p.w)) / 512, 1);
    return std::abs(static_cast<i64>(p.x) - side * static_cast<i64>(p.w)) <= tolerance;
  };
  bool left = false, right = false;
  for (std::size_t i = 0; i < size; ++i) {
    left = left || on_side(v[i], -1);
    right = right || on_side(v[i], 1);
  }
  if (!left || !right) return;
  // How each part changes with x and y: from the three corners that span
  // the most.
  std::size_t a = 0, b = 1, c = 2;
  double best = 0;
  for (std::size_t i = 0; i < size; ++i)
    for (std::size_t j = i + 1; j < size; ++j)
      for (std::size_t k = j + 1; k < size; ++k) {
        const double area = std::abs(static_cast<double>(v[j].x - v[i].x) * (v[k].y - v[i].y) -
                                     static_cast<double>(v[k].x - v[i].x) * (v[j].y - v[i].y));
        if (area > best) {
          best = area;
          a = i, b = j, c = k;
        }
      }
  if (best == 0) return;
  const double x1 = v[b].x - v[a].x, y1 = v[b].y - v[a].y, x2 = v[c].x - v[a].x, y2 = v[c].y - v[a].y;
  const double det = x1 * y2 - x2 * y1;
  // The change of a part per unit of x, from its values at the three.
  const auto per_x = [&](double pa, double pb, double pc) { return ((pb - pa) * y2 - (pc - pa) * y1) / det; };
  const Vertex old[3] = {v[a], v[b], v[c]};
  for (std::size_t i = 0; i < size; ++i) {
    Vertex& p = v[i];
    const int side = on_side(p, -1) ? -1 : on_side(p, 1) ? 1 : 0;
    if (side == 0) continue;
    const i32 to = static_cast<i32>(side * 4 * static_cast<i64>(p.w) / 3);
    const double dx = static_cast<double>(to) - p.x;
    const auto moved = [&](double value, double change) { return value + change * dx; };
    p.z = static_cast<i32>(std::lround(moved(p.z, per_x(old[0].z, old[1].z, old[2].z))));
    p.s = static_cast<std::int16_t>(
        std::clamp<long>(std::lround(moved(p.s, per_x(old[0].s, old[1].s, old[2].s))), -32768, 32767));
    p.t = static_cast<std::int16_t>(
        std::clamp<long>(std::lround(moved(p.t, per_x(old[0].t, old[1].t, old[2].t))), -32768, 32767));
    u32 colour = 0;
    for (unsigned shift = 0; shift < 18; shift += 6) {
      const auto part = [&](const Vertex& q) { return static_cast<double>((q.colour >> shift) & 0x3F); };
      const long value = std::lround(moved(part(p), per_x(part(old[0]), part(old[1]), part(old[2]))));
      colour |= static_cast<u32>(std::clamp<long>(value, 0, 63)) << shift;
    }
    p.colour = colour;
    p.x = to;
  }
}

// Cuts a polygon down to the view volume (each of x, y, z between -w and w),
// one side after the other. `v` holds `size` corners and has room for ten:
// each side can add one. Returns whether anything was cut off; `size` is
// zero when nothing is left. `wide`: x between -4/3 w and 4/3 w, for a
// picture a third wider than the DS's (GeometryEngine::set_wide); or, with
// `times` and `over`, between -times/over w and times/over w.
bool cut_to_view(Vertex* v, std::size_t& size, bool wide = false, i64 times = 4, i64 over = 3) {
  bool cut = false;
  Vertex next[10];
  for (int side = 0; side < 6 && size != 0; ++side) {
    // How far inside the side a corner is: 0 or more inside.
    const auto distance = [side, wide, times, over](const Vertex& p) -> i64 {
      const i32 c = side < 2 ? p.x : side < 4 ? p.y : p.z;
      // Negated in 32 bits, as the hardware's arithmetic wraps.
      const i64 value = (side & 1) != 0 ? static_cast<i32>(0u - static_cast<u32>(c)) : c;
      return wide && side < 2 ? over * value + times * static_cast<i64>(p.w) : value + p.w;
    };
    // A side that every corner is inside leaves the polygon as it is: most
    // polygons are in view, and so are spared the copying below.
    if (std::all_of(v, v + size, [&](const Vertex& p) { return distance(p) >= 0; })) continue;
    std::size_t kept = 0;
    for (std::size_t j = 0; j < size; ++j) {
      const Vertex& current = v[j];
      const Vertex& previous = v[(j + size - 1) % size];
      const i64 current_distance = distance(current), previous_distance = distance(previous);
      const bool current_in = current_distance >= 0;
      const bool previous_in = previous_distance >= 0;
      if (current_in) {
        if (!previous_in) {
          next[kept++] = crossing(current, previous, current_distance, previous_distance);
          cut = true;
        }
        next[kept++] = current;
      } else if (previous_in) {
        next[kept++] = crossing(current, previous, current_distance, previous_distance);
        cut = true;
      }
    }
    size = kept;
    std::copy(next, next + size, v);
  }
  return cut;
}

// Five bits of red, green and blue made six each, the way the 3D hardware
// does it: doubled, and one more unless zero.
u32 six_bits_each(u32 colour) {
  u32 result = 0;
  for (unsigned i = 0; i < 3; ++i) {
    u32 part = ((colour >> (5 * i)) & 0x1F) * 2;
    if (part != 0) ++part;
    result |= part << (6 * i);
  }
  return result;
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
  io.add(kCounts, 4, [this] { return counts(); }, nullptr);
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

const Matrix& GeometryEngine::clip() {
  if (clip_stale_) {
    clip_ = times(position_, projection_);
    clip_stale_ = false;
  }
  return clip_;
}

std::uint32_t GeometryEngine::counts() const {
  return static_cast<u32>(building_.vertices.size()) << 16 | static_cast<u32>(building_.polygons.size());
}

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
  if (queue_.empty() && !stopped_ && parameters(entry.command) <= 1) {
    // Carried out at once, as run() would: the queue is as it was, empty, so
    // its status and interrupt level stay as changed() last left them.
    frame_log::Scope frame_log_scope(frame_log::kGeometry);
    ++statistics_.commands[entry.command];
    execute(entry.command, &entry.parameter);
    return;
  }
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
    hand_over_frame();
    run();
  }
  changed();
}

void GeometryEngine::run() {
  if (stopped_ || queue_.empty()) return;
  frame_log::Scope frame_log_scope(frame_log::kGeometry);
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
  clip_stale_ = true;
  switch (mode_) {
    case 0: projection_ = m; break;
    case 1: position_ = m; break;
    case 2: position_ = vector_ = m; break;
    default: texture_ = m; break;
  }
}

void GeometryEngine::multiply(const Matrix& m, bool vector_too) {
  clip_stale_ = true;
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
  clip_stale_ = true;
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
  clip_stale_ = true;
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
  Vertex v[8];
  for (int i = 0; i < 8; ++i) {
    v[i] = at(c[corners[i][0]], c[corners[i][1]], c[corners[i][2]], kOne);
    through(v[i], clip());
  }
  static constexpr std::uint8_t faces[6][4] = {{0, 1, 4, 2}, {3, 5, 7, 6}, {3, 5, 1, 0},
                                               {6, 7, 4, 2}, {0, 3, 6, 2}, {1, 5, 7, 4}};
  flags_ &= ~kBoxInView;
  for (const auto& face : faces) {
    // The box is in view when anything of one of its faces is.
    Vertex corner[10];
    for (int i = 0; i < 4; ++i) corner[i] = v[face[i]];
    std::size_t size = 4;
    // In a wide frame (set_wide), a view wider still than the wide
    // picture's: a game that leaves out what the test finds out of view (a
    // field's models) shows them at the sides too, and one only just past
    // the sides (its box larger than what it draws, or moving in) does not
    // appear late at the edge.
    cut_to_view(corner, size, wide_, kWideBoxTimes, kWideBoxOver);
    if (size != 0) {
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

    case kColour:
      colour_ = six_bits_each(p[0]);
      break;
    case kTextureCoordinates:
      raw_s_ = low16(p[0]);
      raw_t_ = high16(p[0]);
      if ((texture_parameters_ >> 30) == 1) {
        // Through the texture matrix, as the point (s, t, 1/16, 1/16).
        const auto column = [&](int c) {
          return static_cast<std::int16_t>((static_cast<i64>(raw_s_) * texture_[c] + static_cast<i64>(raw_t_) * texture_[4 + c] +
                                            texture_[8 + c] + texture_[12 + c]) >>
                                           12);
        };
        s_ = column(0);
        t_ = column(1);
      } else {
        s_ = raw_s_;
        t_ = raw_t_;
      }
      break;

    case kVertex16:
      vertex_ = {low16(p[0]), high16(p[0]), low16(p[1])};
      add_vertex();
      break;
    case kVertex10:
      // Ten bits each, in units 64 times as large.
      for (unsigned i = 0; i < 3; ++i) vertex_[i] = static_cast<std::int16_t>(ten_bits(p[0], 10 * i) * 64);
      add_vertex();
      break;
    case kVertexXY:
      vertex_[0] = low16(p[0]);
      vertex_[1] = high16(p[0]);
      add_vertex();
      break;
    case kVertexXZ:
      vertex_[0] = low16(p[0]);
      vertex_[2] = high16(p[0]);
      add_vertex();
      break;
    case kVertexYZ:
      vertex_[1] = low16(p[0]);
      vertex_[2] = high16(p[0]);
      add_vertex();
      break;
    case kVertexDifference:
      // Ten bits each, added to the position, wrapping in 16 bits.
      for (unsigned i = 0; i < 3; ++i) vertex_[i] = static_cast<std::int16_t>(vertex_[i] + ten_bits(p[0], 10 * i));
      add_vertex();
      break;

    case kNormal:
      normal(p[0]);
      break;
    case kDiffuseAmbient:
      diffuse_ = six_bits_each(p[0]);
      ambient_ = six_bits_each(p[0] >> 16);
      // Bit 15 makes the diffuse colour the vertex colour as well.
      if ((p[0] & 0x8000) != 0) colour_ = diffuse_;
      break;
    case kSpecularEmission:
      specular_ = six_bits_each(p[0]);
      emission_ = six_bits_each(p[0] >> 16);
      shininess_table_on_ = (p[0] & 0x8000) != 0;
      break;
    case kLightVector: {
      // Ten bits each, in units eight times as large, through the vector
      // matrix as it is now. The line of sight is (0, 0, -1).
      Light& light = lights_[p[0] >> 30];
      Vertex d = at(ten_bits(p[0], 0) * 8, ten_bits(p[0], 10) * 8, ten_bits(p[0], 20) * 8, 0);
      through(d, vector_);
      light.direction = {d.x, d.y, d.z};
      light.half = {d.x / 2, d.y / 2, static_cast<i32>((static_cast<i64>(d.z) - kOne) / 2)};
      break;
    }
    case kLightColour:
      lights_[p[0] >> 30].colour = six_bits_each(p[0]);
      break;
    case kShininess:
      // 128 values, four to a word, the lowest byte first.
      for (unsigned i = 0; i < 128; ++i) shininess_[i] = static_cast<std::uint8_t>(p[i / 4] >> (8 * (i % 4)));
      break;

    case kPolygonAttributes:
      // Takes effect when the next vertex list begins.
      attributes_next_ = p[0];
      break;
    case kTextureParameters:
      texture_parameters_ = p[0];
      break;
    case kPaletteBase:
      palette_ = p[0] & 0x1FFF;
      break;

    case kBeginVertices:
      begin_vertices(p[0]);
      break;
    case kEndVertices:
      // Nothing: a vertex list ends when the next begins, or the frame does.
      break;

    case kSwapBuffers:
      swap_parameter_ = p[0];
      stopped_ = true;
      break;
    case kViewport: {
      // Two corners, the bottom left first, with y counted from the bottom.
      // It takes effect when the next vertex list begins.
      viewport_ = p[0];
      const u32 x1 = p[0] & 0xFF, y1 = (p[0] >> 8) & 0xFF, x2 = (p[0] >> 16) & 0xFF, y2 = p[0] >> 24;
      const u32 top = (191 - y2) & 0xFF;
      viewport_next_ = {static_cast<std::uint16_t>(x1), static_cast<std::uint16_t>(top),
                        static_cast<std::uint16_t>((x2 - x1 + 1) & 0x1FF),
                        static_cast<std::uint16_t>((191 - y1 - top + 1) & 0xFF)};
      break;
    }

    case kBoxTest:
      box_test(p);
      break;
    case kPositionTest: {
      // The position also becomes the current vertex position.
      vertex_ = {low16(p[0]), high16(p[0]), low16(p[1])};
      Vertex r = at(vertex_[0], vertex_[1], vertex_[2], kOne);
      through(r, clip());
      position_result_ = {r.x, r.y, r.z, r.w};
      break;
    }
    case kVectorTest: {
      // Ten bits each, in units eight times as large; through the vector
      // matrix's 3 by 3; the answers keep 13 bits, sign included.
      Vertex r = at(ten_bits(p[0], 0) * 8, ten_bits(p[0], 10) * 8, ten_bits(p[0], 20) * 8, 0);
      through(r, vector_);
      const i32 out[3] = {r.x, r.y, r.z};
      for (unsigned i = 0; i < 3; ++i) {
        const u32 bits = static_cast<u32>(out[i]) & 0x1FFF;
        vector_result_[i] = static_cast<std::int16_t>((bits & 0x1000) != 0 ? static_cast<i32>(bits) - 0x2000 : static_cast<i32>(bits));
      }
      break;
    }

    default:
      if (!exists(command)) ++statistics_.unknown_commands;
      break;
  }
}

void GeometryEngine::normal(std::uint32_t parameter) {
  // Ten bits each, in units eight times as large.
  Vertex n = at(ten_bits(parameter, 0) * 8, ten_bits(parameter, 10) * 8, ten_bits(parameter, 20) * 8, 0);

  if ((texture_parameters_ >> 30) == 2) {
    // Texture coordinates from the normal: through the texture matrix's
    // 3 by 3, added to the coordinates sent.
    const auto column = [&](int c) {
      return (static_cast<i64>(n.x) * texture_[c] + static_cast<i64>(n.y) * texture_[4 + c] +
              static_cast<i64>(n.z) * texture_[8 + c]) >>
             24;
    };
    s_ = static_cast<std::int16_t>(column(0) + raw_s_);
    t_ = static_cast<std::int16_t>(column(1) + raw_t_);
  }

  // The vertex colour: what the material gives off by itself, and for each
  // light that the polygon attributes turn on, what the material throws
  // back of it. How much depends on the angle between the normal and the
  // light (diffuse), on the angle between the normal and the direction half
  // way between light and eye (specular), or on neither (ambient).
  through(n, vector_);
  const auto level = [&](const std::array<i32, 3>& direction) {
    const i64 dot = (static_cast<i64>(direction[0]) * n.x + static_cast<i64>(direction[1]) * n.y +
                     static_cast<i64>(direction[2]) * n.z) >>
                    12;
    return std::clamp<i64>(-dot, 0, kOne);
  };
  u32 colour = emission_;
  for (unsigned i = 0; i < 4; ++i) {
    if ((attributes_ & (1u << i)) == 0) continue;
    const Light& light = lights_[i];
    const i64 diffuse_level = level(light.direction);
    i64 shininess_level = level(light.half);
    shininess_level = (shininess_level * shininess_level) >> 12;
    if (shininess_table_on_) shininess_level = i64{shininess_[std::min<i64>(shininess_level >> 5, 127)]} << 4;
    u32 lit = 0;
    for (unsigned shift = 0; shift < 18; shift += 6) {
      const auto part = [&](u32 c) { return static_cast<i64>((c >> shift) & 0x3F); };
      i64 value = part(colour);
      value += (part(specular_) * part(light.colour) * shininess_level) >> 18;
      value += (part(diffuse_) * part(light.colour) * diffuse_level) >> 18;
      value += (part(ambient_) * part(light.colour)) >> 6;
      lit |= static_cast<u32>(std::clamp<i64>(value, 0, 63)) << shift;
    }
    colour = lit;
  }
  colour_ = colour;
}

void GeometryEngine::begin_vertices(std::uint32_t parameter) {
  forget_loose_vertices();
  place_vertices();
  list_kind_ = parameter & 3;
  list_vertices_ = 0;
  strip_clockwise_ = false;
  attributes_ = attributes_next_;
}

void GeometryEngine::forget_loose_vertices() {
  // Vertices at the end of a list that made no polygon: those of an
  // unfinished polygon, or the two a strip that was broken went on with.
  std::size_t loose = 0;
  switch (list_kind_) {
    case kTriangles: loose = list_vertices_ % 3; break;
    case kQuads: loose = list_vertices_ % 4; break;
    case kTriangleStrip: loose = list_vertices_ < 3 ? list_vertices_ : 0; break;
    default: loose = list_vertices_ < 4 ? list_vertices_ : list_vertices_ % 2; break;
  }
  loose = std::min(loose, building_.vertices.size() - placed_);
  building_.vertices.resize(building_.vertices.size() - loose);
  list_vertices_ = 0;
}

void GeometryEngine::place_vertices() {
  const i64 left = viewport_now_[0], top = viewport_now_[1], width = viewport_now_[2], height = viewport_now_[3];
  for (std::size_t i = placed_; i < building_.vertices.size(); ++i) {
    Vertex& v = building_.vertices[i];
    if (v.w == 0) continue;
    const i64 w = v.w;
    // x is 9 bits and y 8, and they wrap; y runs down the screen.
    // In sixteenths, then whole pixels: (a / b) / 16 is a / (16 b), so the
    // second is the first cut down, as if it were worked out by itself.
    const i64 sixteenths_x = (v.x + w) * width * 16 / (w * 2), sixteenths_y = (w - v.y) * height * 16 / (w * 2);
    v.fine_x = static_cast<i32>((sixteenths_x + left * 16) & 0x1FFF);
    v.fine_y = static_cast<i32>((sixteenths_y + top * 16) & 0xFFF);
    v.x = static_cast<i32>((sixteenths_x / 16 + left) & 0x1FF);
    v.y = static_cast<i32>((sixteenths_y / 16 + top) & 0xFF);
    v.z = static_cast<i32>(((static_cast<i64>(v.z) * (1 << 14)) / w + 0x3FFF) * (1 << 9));
  }
  placed_ = building_.vertices.size();
  // The wide frame's, the same way, but x does not wrap: it goes past the
  // DS's screen on both sides.
  for (std::size_t i = wide_placed_; i < wide_building_.vertices.size(); ++i) {
    Vertex& v = wide_building_.vertices[i];
    if (v.w == 0) continue;
    const i64 w = v.w;
    const i64 sixteenths_x = (v.x + w) * width * 16 / (w * 2), sixteenths_y = (w - v.y) * height * 16 / (w * 2);
    v.fine_x = static_cast<i32>(sixteenths_x + left * 16);
    v.fine_y = static_cast<i32>((sixteenths_y + top * 16) & 0xFFF);
    v.x = static_cast<i32>(sixteenths_x / 16 + left);
    v.y = static_cast<i32>((sixteenths_y / 16 + top) & 0xFF);
    v.z = static_cast<i32>(((static_cast<i64>(v.z) * (1 << 14)) / w + 0x3FFF) * (1 << 9));
  }
  wide_placed_ = wide_building_.vertices.size();
  viewport_now_ = viewport_next_;
}

void GeometryEngine::hand_over_frame() {
  frame_log::Scope frame_log_scope(frame_log::kGeometry);
  forget_loose_vertices();
  place_vertices();
  placed_ = 0;
  statistics_.most_vertices = std::max<std::uint64_t>(statistics_.most_vertices, building_.vertices.size());
  statistics_.most_polygons = std::max<std::uint64_t>(statistics_.most_polygons, building_.polygons.size());
  building_.swap_parameter = swap_parameter_;
  std::swap(building_, finished_);
  building_.vertices.clear();
  building_.polygons.clear();
  building_.depth_is_w = (swap_parameter_ & 2) != 0;
  wide_building_.swap_parameter = finished_.swap_parameter;
  wide_building_.depth_is_w = finished_.depth_is_w;
  std::swap(wide_building_, wide_finished_);
  wide_building_.vertices.clear();
  wide_building_.polygons.clear();
  wide_building_.depth_is_w = building_.depth_is_w;
  wide_placed_ = 0;
  list_vertices_ = 0;
}

void GeometryEngine::add_vertex() {
  std::vector<Vertex>& vertices = building_.vertices;
  ++statistics_.vertices;
  if (vertices.size() >= kMostVertices) {
    ++statistics_.vertices_without_room;
    if (on_no_room) on_no_room();
    return;
  }
  Vertex v = at(vertex_[0], vertex_[1], vertex_[2], kOne);
  v.s = s_;
  v.t = t_;
  v.colour = colour_;
  if ((texture_parameters_ >> 30) == 3) {
    // Texture coordinates from the vertex: its position through the texture
    // matrix's 3 by 3, added to the coordinates sent.
    const auto column = [&](int c) {
      return (static_cast<i64>(v.x) * texture_[c] + static_cast<i64>(v.y) * texture_[4 + c] +
              static_cast<i64>(v.z) * texture_[8 + c]) >>
             24;
    };
    v.s = static_cast<std::int16_t>(column(0) + raw_s_);
    v.t = static_cast<std::int16_t>(column(1) + raw_t_);
  }
  const Matrix& m = clip();
  // w is the vertex through the last column: without x, y and z in it, it
  // is the same wherever the vertex is.
  const bool flat = m[3] == 0 && m[7] == 0 && m[11] == 0;
  through(v, m);
  vertices.push_back(v);
  recent_flat_ = (recent_flat_ << 1) | (flat ? 1u : 0u);
  ++list_vertices_;

  switch (list_kind_) {
    case kTriangles:
      if (list_vertices_ % 3 == 0) add_polygon();
      break;
    case kQuads:
      if (list_vertices_ % 4 == 0) add_polygon();
      break;
    case kTriangleStrip:
      if (list_vertices_ >= 3) add_polygon();
      break;
    default:
      if (list_vertices_ >= 4 && list_vertices_ % 2 == 0) add_polygon();
      break;
  }
}

void GeometryEngine::add_polygon() {
  std::vector<Vertex>& vertices = building_.vertices;
  if (building_.polygons.size() >= kMostPolygons) {
    ++statistics_.polygons_without_room;
    if (on_no_room) on_no_room();
    return;
  }
  const std::size_t corners = 3 + (list_kind_ & 1);
  if (vertices.size() < corners) return;
  const std::size_t count = vertices.size();

  // The corners, in the order they run around the polygon. A strip of
  // four-cornered polygons sends the last two the other way round.
  Vertex corner[10];
  std::copy(vertices.end() - static_cast<std::ptrdiff_t>(corners), vertices.end(), corner);
  if (list_kind_ == kQuadStrip) std::swap(corner[2], corner[3]);

  // Which way it faces: the cross product of two of its edges, in x, y and
  // w, against its first corner. Above zero is the front.
  const i64 x1 = static_cast<i64>(corner[1].x) - corner[0].x, x2 = static_cast<i64>(corner[2].x) - corner[0].x;
  const i64 y1 = static_cast<i64>(corner[1].y) - corner[0].y, y2 = static_cast<i64>(corner[2].y) - corner[0].y;
  const i64 w1 = static_cast<i64>(corner[1].w) - corner[0].w, w2 = static_cast<i64>(corner[2].w) - corner[0].w;
  i64 xc = wrapping_minus(wrapping_times(y1, w2), wrapping_times(w1, y2));
  i64 yc = wrapping_minus(wrapping_times(w1, x2), wrapping_times(x1, w2));
  i64 wc = wrapping_minus(wrapping_times(x1, y2), wrapping_times(y1, x2));
  while (xc != static_cast<i32>(xc) || yc != static_cast<i32>(yc) || wc != static_cast<i32>(wc)) {
    xc >>= 4;
    yc >>= 4;
    wc >>= 4;
  }
  i64 facing = wrapping_plus(wrapping_plus(wrapping_times(xc, corner[0].x), wrapping_times(yc, corner[0].y)),
                             wrapping_times(wc, corner[0].w));

  Polygon polygon;
  polygon.clockwise = facing < 0;
  polygon.attributes = attributes_;
  polygon.texture = texture_parameters_;
  polygon.palette = palette_;
  const std::uint32_t corner_bits = (1u << corners) - 1;
  polygon.flat = (recent_flat_ & corner_bits) == corner_bits;

  // A strip of triangles stores every other one the other way round.
  if (list_kind_ == kTriangleStrip) {
    if (strip_clockwise_) facing = wrapping_minus(0, facing);
    strip_clockwise_ = !strip_clockwise_;
  }

  std::size_t size = corners;
  bool cut = false;
  if (((attributes_ & kDrawFront) == 0 && facing > 0) || ((attributes_ & kDrawBack) == 0 && facing < 0)) {
    ++statistics_.polygons_facing_away;
    size = 0;
  } else if ((attributes_ & kCutAtFarSide) == 0 &&
             std::any_of(corner, corner + corners, [](const Vertex& v) { return v.z > v.w; })) {
    ++statistics_.polygons_past_far_side;
    size = 0;
  } else {
    if (wide_) add_wide_polygon(polygon, corner, corners);
    cut = cut_to_view(corner, size);
    if (size == 0) ++statistics_.polygons_out_of_view;
  }

  if (size == 0) {
    // Dropped, and its vertices with it as far as no other polygon has them.
    switch (list_kind_) {
      case kTriangles:
      case kQuads:
        vertices.resize(count - corners);
        break;
      case kTriangleStrip:
        if (list_vertices_ == 3) {
          // The strip's first: the strip goes on from its last two vertices.
          vertices[count - 3] = vertices[count - 2];
          vertices[count - 2] = vertices[count - 1];
          vertices.pop_back();
          --list_vertices_;
        } else if (count < kMostVertices) {
          // The strip is broken: a new one starts with the last two
          // vertices, the first of which the triangle before still uses.
          vertices.push_back(vertices[count - 1]);
          vertices[count - 1] = vertices[count - 2];
          list_vertices_ = 2;
        }
        break;
      default:
        if (list_vertices_ == 4) {
          vertices[count - 4] = vertices[count - 2];
          vertices[count - 3] = vertices[count - 1];
          vertices.resize(count - 2);
          list_vertices_ -= 2;
        } else {
          list_vertices_ = 2;
        }
        break;
    }
    return;
  }

  polygon.first = static_cast<std::uint16_t>(count - corners);
  if (cut) {
    // The cut polygon's corners take the place of the vertices sent. A strip
    // is broken by it: the polygon before keeps the vertices it shares, and
    // a new strip starts with copies of the last two.
    const auto append = [&](const Vertex& v) {
      if (vertices.size() >= kMostVertices) {
        ++statistics_.vertices_without_room;
        if (on_no_room) on_no_room();
        return false;
      }
      vertices.push_back(v);
      return true;
    };
    const bool strip = list_kind_ == kTriangleStrip || list_kind_ == kQuadStrip;
    // A new strip goes on from the last two vertices sent, in that order.
    const Vertex last_but_one = vertices[count - 2], last = vertices[count - 1];
    const std::size_t shared = strip && list_vertices_ != corners ? 2 : 0;
    vertices.resize(count - corners + shared);
    polygon.first = static_cast<std::uint16_t>(vertices.size());
    for (std::size_t i = 0; i < size; ++i)
      if (!append(corner[i])) return;
    if (strip) {
      if (!append(last_but_one) || !append(last)) return;
      list_vertices_ = 2;
    }
    ++statistics_.polygons_cut;
  }
  polygon.size = static_cast<std::uint8_t>(size);
  polygon.strip_order = list_kind_ == kQuadStrip && !cut;
  building_.polygons.push_back(polygon);
  ++statistics_.polygons;
}

void GeometryEngine::add_wide_polygon(const Polygon& polygon, const Vertex* corners, std::size_t count) {
  Vertex corner[10];
  std::copy(corners, corners + count, corner);
  std::size_t size = count;
  // Drawn with perspective: as far as the wider picture shows. Without (a
  // HUD, a menu): as on the DS, as such polygons are placed on its screen;
  // but one across the whole screen (a box behind a message) goes on to
  // the wider picture's sides.
  cut_to_view(corner, size, !polygon.flat);
  if (polygon.flat && size >= 3) widen_across(corner, size);
  if (size == 0 || wide_building_.polygons.size() >= kMostWidePolygons ||
      wide_building_.vertices.size() + size > kMostWideVertices)
    return;
  Polygon wide = polygon;
  wide.first = static_cast<std::uint16_t>(wide_building_.vertices.size());
  wide.size = static_cast<std::uint8_t>(size);
  wide.strip_order = false;
  wide_building_.vertices.insert(wide_building_.vertices.end(), corner, corner + size);
  wide_building_.polygons.push_back(wide);
}

}  // namespace bd::nds
