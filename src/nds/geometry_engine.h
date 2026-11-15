// The front half of the DS's 3D hardware: the queue of commands a program
// feeds it, the matrices those commands work on, and the status register the
// program watches.
//
// A program draws in 3D by sending commands: set a matrix, multiply it, send
// a vertex, begin and end a polygon list, and at the end of a frame "swap
// buffers". Commands arrive through one port, packed four numbers to a word
// and followed by their parameters, or through a port of their own each.
// They go through a queue of 256 entries. "Swap buffers" stops the engine
// until the next vertical blank, and what is sent meanwhile waits in the
// queue; when the queue is full, the processor is made to wait.
//
// What is here:
// - Both ways in, the queue, and the status register: how full the queue is,
//   whether the engine is busy, the matrix stack levels and error flag.
// - The interrupt on "queue less than half full" or "queue empty". It is a
//   level: the request stands for as long as the condition does.
// - The four matrices (projection, position, vector, texture), their stacks,
//   and every matrix command, in the hardware's 20.12 fixed point.
// - The three tests a program can ask for: a position through the matrices,
//   a vector through the vector matrix, and whether a box is in view.
// - The registers that read the matrices back.
// - The current vertex position, which the vertex commands change.
//
// What is not, yet: vertices are not turned into polygons, so nothing is
// lit, textured, clipped or counted. The commands for that are accepted and
// have no effect, and the register that tells how many polygons and vertices
// a frame has reads as zero.
//
// Commands take no time here: each is carried out the moment its last
// parameter arrives, unless the engine is stopped for a swap. On the
// hardware each takes a few cycles to a few hundred, so a program sees the
// queue fill and the engine busy more often than it does here.
//
// What the commands do is GBATEK's description; where that leaves details
// open (the edges of the matrix stacks, the order of a box's faces, how the
// queue hands entries on) NooDS is followed. The hardware takes up to four
// entries out of the queue before it runs them; here that is: the first four
// waiting entries do not count as queued.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"

namespace bd::nds {

class GeometryEngine {
 public:
  // Sixteen numbers, row by row: 20.12 fixed point. A point is a row that is
  // multiplied by the matrix from the left.
  using Matrix = std::array<std::int32_t, 16>;
  static constexpr std::int32_t kOne = 1 << 12;
  static constexpr Matrix kIdentity = {kOne, 0, 0, 0, 0, kOne, 0, 0, 0, 0, kOne, 0, 0, 0, 0, kOne};

  static constexpr std::uint32_t kFifoPort = 0x04000400;     // And fifteen more words that are the same port.
  static constexpr std::uint32_t kCommandPorts = 0x04000400; // Command n has its own port at this + 4 n.
  static constexpr std::uint32_t kStatus = 0x04000600;
  static constexpr std::uint32_t kCounts = 0x04000604;       // Polygons and vertices in the frame being built.
  static constexpr std::uint32_t kPositionResult = 0x04000620;  // Four words.
  static constexpr std::uint32_t kVectorResult = 0x04000630;    // Three 16-bit values.
  static constexpr std::uint32_t kClipMatrix = 0x04000640;      // Sixteen words: position times projection.
  static constexpr std::uint32_t kVectorMatrix = 0x04000680;    // Nine words: the vector matrix's 3 by 3.

  static constexpr std::size_t kQueueSize = 256;

  // Commands.
  enum Command : unsigned {
    kMatrixMode = 0x10,
    kMatrixPush = 0x11,
    kMatrixPop = 0x12,
    kMatrixStore = 0x13,
    kMatrixRestore = 0x14,
    kMatrixIdentity = 0x15,
    kMatrixLoad4x4 = 0x16,
    kMatrixLoad4x3 = 0x17,
    kMatrixMultiply4x4 = 0x18,
    kMatrixMultiply4x3 = 0x19,
    kMatrixMultiply3x3 = 0x1A,
    kMatrixScale = 0x1B,
    kMatrixTranslate = 0x1C,
    kColour = 0x20,
    kNormal = 0x21,
    kTextureCoordinates = 0x22,
    kVertex16 = 0x23,
    kVertex10 = 0x24,
    kVertexXY = 0x25,
    kVertexXZ = 0x26,
    kVertexYZ = 0x27,
    kVertexDifference = 0x28,
    kPolygonAttributes = 0x29,
    kTextureParameters = 0x2A,
    kPaletteBase = 0x2B,
    kDiffuseAmbient = 0x30,
    kSpecularEmission = 0x31,
    kLightVector = 0x32,
    kLightColour = 0x33,
    kShininess = 0x34,
    kBeginVertices = 0x40,
    kEndVertices = 0x41,
    kSwapBuffers = 0x50,
    kViewport = 0x60,
    kBoxTest = 0x70,
    kPositionTest = 0x71,
    kVectorTest = 0x72,
  };
  // How many parameter words a command takes. Numbers that are no command
  // take none, and do nothing.
  static unsigned parameters(unsigned command);
  static bool exists(unsigned command);
  // What a command is, in a few words, for reports.
  static const char* command_name(unsigned command);

  // Bits of the status register.
  static constexpr std::uint32_t kTestBusy = 1u << 0;
  static constexpr std::uint32_t kBoxInView = 1u << 1;
  static constexpr unsigned kPositionLevelShift = 8;         // Five bits.
  static constexpr std::uint32_t kProjectionLevel = 1u << 13;
  static constexpr std::uint32_t kStackBusy = 1u << 14;
  static constexpr std::uint32_t kStackError = 1u << 15;     // Write a one to clear.
  static constexpr unsigned kQueuedShift = 16;               // Nine bits.
  static constexpr std::uint32_t kLessThanHalfFull = 1u << 25;
  static constexpr std::uint32_t kEmpty = 1u << 26;
  static constexpr std::uint32_t kBusy = 1u << 27;
  static constexpr unsigned kInterruptShift = 30;            // 1: less than half full. 2: empty.

  struct Statistics {
    std::array<std::uint64_t, 256> commands{};  // Carried out, by number.
    std::uint64_t unknown_commands = 0;         // Numbers that are no command.
    std::uint64_t swaps = 0;
    std::uint64_t waits_for_room = 0;           // Times the queue was full when an entry came.
    std::uint64_t most_queued = 0;
  };

  // `interrupts` must outlive this object.
  explicit GeometryEngine(Interrupts& interrupts);

  void attach(Arm9Io& io);

  // A word written to the queue's port, and a parameter written to a
  // command's own port.
  void write_fifo(std::uint32_t word);
  void write_command(unsigned command, std::uint32_t parameter);

  std::uint32_t status() const;
  void write_status(std::uint32_t value, std::uint32_t mask);

  // The vertical blank has begun. A swap that was asked for happens, and the
  // engine goes on with whatever has queued up.
  void vertical_blank();

  // Called when an entry arrives and the queue is full. The caller is to let
  // time pass until room() is true: the processor waits. If it returns with
  // the queue still full, the entry is queued all the same.
  std::function<void()> on_full;
  // Called when the queue has come to be less than half full.
  std::function<void()> on_less_than_half_full;

  std::size_t queued() const;  // Entries waiting, less the four taken ahead.
  bool room() const { return queued() < kQueueSize; }
  bool less_than_half_full() const { return queued() < kQueueSize / 2; }
  bool stopped_for_swap() const { return stopped_; }

  // State, for tests and reports.
  unsigned matrix_mode() const { return mode_; }
  const Matrix& projection() const { return projection_; }
  const Matrix& position() const { return position_; }
  const Matrix& vector() const { return vector_; }
  const Matrix& texture() const { return texture_; }
  Matrix clip_matrix() const;
  const std::array<std::int16_t, 3>& vertex() const { return vertex_; }
  std::uint32_t viewport() const { return viewport_; }
  std::uint32_t swap_parameter() const { return swap_parameter_; }
  const Statistics& statistics() const { return statistics_; }

 private:
  struct Entry {
    std::uint8_t command;
    std::uint32_t parameter;
  };

  void add(Entry entry);
  void run();
  void execute(unsigned command, const std::uint32_t* p);
  void changed();
  // The matrix or matrices the current mode names, multiplied from the left
  // by, or replaced with, `m`.
  void multiply(const Matrix& m, bool vector_too = true);
  void load(const Matrix& m);
  void push();
  void pop(std::uint32_t parameter);
  void store(std::uint32_t parameter);
  void restore(std::uint32_t parameter);
  void box_test(const std::uint32_t* p);

  Interrupts& interrupts_;
  std::deque<Entry> queue_;
  bool stopped_ = false;          // "Swap buffers" has been carried out; waiting for the vertical blank.
  std::uint32_t pack_ = 0;        // Command numbers of the word being worked through,
  unsigned pack_received_ = 0;    // and how many parameters of the first have come.
  unsigned queued_tests_ = 0, queued_stack_commands_ = 0;
  bool was_less_than_half_full_ = true;

  std::uint32_t flags_ = 0;       // The status bits that are kept as such: box result, error, interrupt choice.
  unsigned mode_ = 0;
  Matrix projection_ = kIdentity, position_ = kIdentity, vector_ = kIdentity, texture_ = kIdentity;
  Matrix projection_stack_ = kIdentity, texture_stack_ = kIdentity;
  std::array<Matrix, 32> position_stack_{}, vector_stack_{};
  unsigned position_level_ = 0;
  bool projection_level_ = false;
  std::array<std::int16_t, 3> vertex_{};
  std::array<std::int32_t, 4> position_result_{};
  std::array<std::int16_t, 3> vector_result_{};
  std::uint32_t viewport_ = 0, swap_parameter_ = 0;
  Statistics statistics_;
};

}  // namespace bd::nds
