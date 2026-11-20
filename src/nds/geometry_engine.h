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
// - Vertices into polygons. Each vertex goes through the matrices, and the
//   vertex list's kind (triangles, four-cornered polygons, or strips of
//   either) says which vertices make a polygon. A polygon that faces the
//   wrong way is dropped, one that leaves the view volume is cut down to it,
//   and when a vertex list ends its vertices are placed on the screen by the
//   viewport. The frame being built holds at most 6144 vertices and 2048
//   polygons; a register tells the program how many there are. "Swap
//   buffers" hands the frame over for drawing and starts an empty one.
// - With each vertex its colour and texture coordinates, and with each
//   polygon the attribute, texture and palette words in force.
// - Lighting. A vertex colour is either sent as such or worked out when a
//   normal is sent: from up to four lights, each a direction and a colour,
//   and the material's four colours (diffuse, ambient, specular, emission),
//   with the table that can shape the specular part.
//
// Drawing the finished frame is the back half's business
// (polygon_renderer). So is the flag that tells a program that vertices or
// polygons did not fit: it is in a register there, and this half says when.
//
// Commands take no time here: each is carried out the moment its last
// parameter arrives, unless the engine is stopped for a swap. On the
// hardware each takes a few cycles to a few hundred, so a program sees the
// queue fill and the engine busy more often than it does here.
//
// What the commands do is GBATEK's description; where that leaves details
// open (the edges of the matrix stacks, the order of a box's faces, how the
// queue hands entries on, the arithmetic of lighting and of facing, cutting
// and placing polygons, and which vertices a strip keeps) NooDS is followed,
// with one
// difference: vertices at the end of a list that made no polygon are not
// kept in the frame. The hardware takes up to four
// entries out of the queue before it runs them; here that is: the first four
// waiting entries do not count as queued.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

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
  static constexpr std::size_t kMostVertices = 6144;  // In a frame.
  static constexpr std::size_t kMostPolygons = 2048;

  // A vertex of the frame. Until its vertex list is finished, x, y, z and w
  // are its place after the matrices ("clip space": it is in view when each
  // of x, y and z is between -w and w). Afterwards x and y are its place on
  // the screen, in pixels from the top left, and z is its depth, from about
  // 0 at the near side of the view volume to 0xFFFE00 at the far side; w
  // stays.
  struct Vertex {
    std::int32_t x = 0, y = 0, z = 0, w = 0;
    std::int16_t s = 0, t = 0;   // Texture coordinates, in sixteenths of a texel.
    std::uint32_t colour = 0;    // Six bits each of red, green and blue, red lowest.
    // Once placed, x and y in sixteenths of a pixel, from the same sums
    // before they are cut to whole pixels (x is then fine_x / 16). The DS
    // draws at whole pixels; a renderer that draws larger can use these, so
    // that slowly moving models do not jump from pixel to pixel.
    std::int32_t fine_x = 0, fine_y = 0;

    bool operator==(const Vertex&) const = default;
  };

  // A polygon of the frame: `size` vertices in a row, from `first`.
  struct Polygon {
    std::uint16_t first = 0;
    std::uint8_t size = 0;           // 3 to 10: cutting can add corners.
    bool clockwise = false;          // How its corners run on the screen, as stored.
    // An uncut polygon of a strip of four-cornered polygons has its corners
    // stored in the strip's order: around the polygon they run 0, 1, 3, 2.
    bool strip_order = false;
    std::uint32_t attributes = 0;    // The polygon attribute word in force when its vertex list began.
    std::uint32_t texture = 0;       // The texture parameter word,
    std::uint32_t palette = 0;       // and the texture palette, when it was made.
    // Its corners' w does not depend on where they are: it was drawn with a
    // projection without perspective (orthographic), as a game draws its HUD
    // and menus in 3D. A presentation choice for 3D scenes, such as smoothed
    // textures, leaves such polygons as they are.
    bool flat = false;
  };

  struct Frame {
    std::vector<Vertex> vertices;
    std::vector<Polygon> polygons;
    std::uint32_t swap_parameter = 0;  // Of the "swap buffers" that ended it.
    // Whether depth is to be w rather than z. That is bit 1 of the "swap
    // buffers" before the frame: it counts for the polygons sent after it.
    bool depth_is_w = false;
  };

  // Kinds of vertex list: the parameter of "begin a vertex list".
  enum ListKind : unsigned { kTriangles = 0, kQuads = 1, kTriangleStrip = 2, kQuadStrip = 3 };

  // Bits of the polygon attribute word that matter here.
  static constexpr std::uint32_t kDrawBack = 1u << 6;
  static constexpr std::uint32_t kDrawFront = 1u << 7;
  static constexpr std::uint32_t kCutAtFarSide = 1u << 12;  // Clear: a polygon that crosses the far side is dropped.

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
    std::uint64_t vertices = 0;                 // Sent by the program.
    std::uint64_t polygons = 0;                 // Put into a frame.
    std::uint64_t polygons_cut = 0;             // Of those, cut down to the view volume.
    std::uint64_t polygons_facing_away = 0;     // Dropped: facing a way that is not drawn.
    std::uint64_t polygons_out_of_view = 0;     // Dropped: nothing left after cutting.
    std::uint64_t polygons_past_far_side = 0;   // Dropped: crossing the far side, with cutting there off.
    std::uint64_t vertices_without_room = 0;    // Vertices and polygons that came when the frame was full.
    std::uint64_t polygons_without_room = 0;
    std::uint64_t most_vertices = 0;            // In one frame, when it was handed over.
    std::uint64_t most_polygons = 0;
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
  // What the count register reads: polygons in the frame being built in the
  // low half, vertices in the high half.
  std::uint32_t counts() const;

  // The vertical blank has begun. A swap that was asked for happens, and the
  // engine goes on with whatever has queued up.
  void vertical_blank();

  // Called when an entry arrives and the queue is full. The caller is to let
  // time pass until room() is true: the processor waits. If it returns with
  // the queue still full, the entry is queued all the same.
  std::function<void()> on_full;
  // Called when the queue has come to be less than half full.
  std::function<void()> on_less_than_half_full;
  // Called when a vertex or a polygon comes and the frame is full.
  std::function<void()> on_no_room;

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
  // The frame being built, and the one last handed over for drawing.
  const Frame& building() const { return building_; }
  const Frame& finished() const { return finished_; }
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
  void normal(std::uint32_t parameter);
  const Matrix& clip();
  void begin_vertices(std::uint32_t parameter);
  void add_vertex();
  void add_polygon();
  void forget_loose_vertices();
  // Places the vertices of the list that has ended on the screen.
  void place_vertices();
  void hand_over_frame();

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
  Matrix clip_ = kIdentity;       // Position times projection, when not stale.
  bool clip_stale_ = false;
  std::array<std::int16_t, 3> vertex_{};
  std::int16_t raw_s_ = 0, raw_t_ = 0;  // Texture coordinates as last sent,
  std::int16_t s_ = 0, t_ = 0;          // and as the next vertex gets them.
  std::uint32_t colour_ = 0;
  // Lighting. Colours are six bits a part, as a vertex colour is.
  struct Light {
    std::array<std::int32_t, 3> direction{};  // Through the vector matrix, when it was sent.
    std::array<std::int32_t, 3> half{};       // Half way between that and the line of sight.
    std::uint32_t colour = 0;
  };
  std::array<Light, 4> lights_{};
  std::uint32_t diffuse_ = 0, ambient_ = 0, specular_ = 0, emission_ = 0;
  bool shininess_table_on_ = false;
  std::array<std::uint8_t, 128> shininess_{};
  std::uint32_t attributes_next_ = 0;   // The polygon attribute word as last sent,
  std::uint32_t attributes_ = 0;        // and as in force since the vertex list began.
  std::uint32_t texture_parameters_ = 0, palette_ = 0;
  unsigned list_kind_ = kTriangles;
  // Bit 0 for the last vertex added, bit 1 for the one before, and so on:
  // whether the clip matrix it went through was without perspective.
  std::uint32_t recent_flat_ = 0;
  std::size_t list_vertices_ = 0;       // Vertices of the list so far, or of the strip since it was last broken.
  bool strip_clockwise_ = false;        // A strip of triangles stores every other one the other way round.
  std::size_t placed_ = 0;              // Vertices of the frame already placed on the screen.
  // Left edge, top edge, width and height on the screen: for the vertex list
  // under way, and for the next.
  std::array<std::uint16_t, 4> viewport_now_ = {0, 0, 256, 192}, viewport_next_ = {0, 0, 256, 192};
  Frame building_, finished_;
  std::array<std::int32_t, 4> position_result_{};
  std::array<std::int16_t, 3> vector_result_{};
  std::uint32_t viewport_ = 0, swap_parameter_ = 0;
  Statistics statistics_;
};

}  // namespace bd::nds
