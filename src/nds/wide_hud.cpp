#include "nds/wide_hud.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "nds/frame_2d.h"

namespace bd::nds {

namespace {

using Frame = GeometryEngine::Frame;
using Polygon = GeometryEngine::Polygon;
using Vertex = GeometryEngine::Vertex;

// A rectangle in DS pixels, x0 up to x1 and y0 up to y1.
struct Box {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  // Wholly in `r`, give or take `slack`.
  bool inside(const Box& r, float slack = 0.5f) const {
    return x0 >= r.x0 - slack && x1 <= r.x1 + slack && y0 >= r.y0 - slack && y1 <= r.y1 + slack;
  }
  // Over `r` by more than `by` each way.
  bool overlaps(const Box& r, float by = 1.0f) const {
    return std::min(x1, r.x1) - std::max(x0, r.x0) > by && std::min(y1, r.y1) - std::max(y0, r.y0) > by;
  }
  bool near(const Box& r, float slack) const {
    return std::abs(x0 - r.x0) <= slack && std::abs(x1 - r.x1) <= slack && std::abs(y0 - r.y0) <= slack &&
           std::abs(y1 - r.y1) <= slack;
  }
  void add(const Box& b) {
    x0 = std::min(x0, b.x0);
    y0 = std::min(y0, b.y0);
    x1 = std::max(x1, b.x1);
    y1 = std::max(y1, b.y1);
  }
};

constexpr Box kNothing{1e9f, 1e9f, -1e9f, -1e9f};

// Where a polygon is on the screen (its corners' fine places).
Box box_of(const Frame& frame, const Polygon& p) {
  Box b = kNothing;
  for (unsigned k = 0; k < p.size; ++k) {
    const Vertex& v = frame.vertices[p.first + k];
    const float x = static_cast<float>(v.fine_x) / 16.0f, y = static_cast<float>(v.fine_y) / 16.0f;
    b.add(Box{x, y, x, y});
  }
  return b;
}

// --- The elements, as measured on the screen (DS pixels).

// A box that moves to a side of the wide screen: its frame is flat 3D
// polygons wholly in `region`, together reaching `shape` (each edge within
// kShapeSlack; with `any_height`, the bottom anywhere in the region: it
// grows with what it says). Its text and icons in the region go with it.
struct BoxRule {
  Box region;
  Box shape;
  bool any_height;
  int direction;  // -1 to the left side, 1 to the right.
};
constexpr float kShapeSlack = 2.5f;
// At least this many pieces (a frame is cut into nine).
constexpr unsigned kLeastPieces = 3;
// The coins box at the left (the menu, a shop): "Coins" over the amount.
constexpr BoxRule kCoinsBox{{0, 99, 72, 137}, {0, 101, 64, 135}, false, -1};
// The price box at the right (a shop): "Price" over the amount.
constexpr BoxRule kPriceBox{{184, 99, 256, 137}, {192, 102, 256, 135}, false, 1};
// The item's information at the right (a shop): what it does, its numbers.
constexpr BoxRule kItemInfoBox{{128, 44, 256, 106}, {136, 51, 248, 104}, true, 1};

// The label over the ring of commands (what the chosen command is): it
// stays in the middle. Its pieces are wholly in kLabelRegion.
constexpr Box kLabelRegion{88, 104, 168, 134};

// The ring of commands (the menu, a battle, a shop): a bar across the
// screen with round ends, its icons inside, the chosen one in the middle
// under a label. Its pieces are wholly in kRingRegion; an end is a piece of
// the frame at a screen edge (kRingEndWidth across, kRingEndHeight high at
// least). Made longer: the ends move to the wide screen's edges and the
// straight parts beside them go on into the sides; the icons and the label
// stay where the game puts them, and icons past the DS's screen are shown
// as far as the bar goes.
constexpr Box kRingRegion{0, 117, 256, 169};
constexpr float kRingEndLeast = 20, kRingEndMost = 50, kRingEndHeight = 15;
// Icons past the DS's screen, in the longer ring: wholly in this.
constexpr Box kRingBeyondRegion{-64, 117, 320, 169};

// The help bar at the bottom (the menu, a shop, a battle's commands): a
// text box from the left edge, and in the menu the B button at its right.
// Made longer: its left end and its text move to the left side, its right
// end and the B button to the right side, and its middle goes on between.
// An end reaches the bottom (kHelpBottom) and is at most kHelpEndMost wide.
constexpr Box kHelpRegion{0, 157, 256, 193};
constexpr float kHelpBottom = 191.5f, kHelpEndMost = 16;

// The band behind a battle's messages: a flat polygon across the wide
// picture (GeometryEngine widens such a band) at the bottom, from about
// kLogTopLeast down, with at least kLogFadeLines thin ones over it (at most
// kLogFadeHeight high: it fades in at the top). Its text (the message)
// moves to the left side.
constexpr float kLogTopLeast = 150, kLogTopMost = 172;
constexpr unsigned kLogFadeLines = 2;
constexpr float kLogFadeHeight = 2;
// The mark at the end of the message (a small turning gem): the polygons
// drawn after the band, together at most kLogMarkSize across and high, in
// the band. It moves with the message.
constexpr float kLogMarkSize = 16;

// The party's names and numbers at the top right (the menu, a battle):
// text alone, no 3D. Moved to the right side if the text layer has pixels
// in kStatsArea and none just left of it or just under it (nothing that
// goes on past it), and no flat polygon is over it.
constexpr Box kStatsArea{150, 0, 256, 34};
constexpr Box kStatsLeftOf{138, 0, 150, 34}, kStatsUnder{150, 34, 256, 42};

// The field's shortcuts (sprites, on the touch screen): L at the top left,
// R at the top right, the map (Y) at the bottom left, the compass and
// settings (X) at the bottom right. A shortcut is the sprites wholly in its
// region; it moves only if some are, and none is partly in it; and only
// where no box or bar of the menus is on the screen.
struct SpriteRule {
  Box region;
  int direction;
};
constexpr SpriteRule kFieldShortcuts[] = {
    {{-32, -12, 48, 32}, -1},    // L
    {{208, -12, 288, 32}, 1},    // R
    {{-8, 152, 40, 196}, -1},    // Y, the map
    {{216, 152, 264, 196}, 1},   // X, the compass and settings
};

// A polygon's corners moved across, each by its own amount; where they move
// by different amounts (a bar's middle, made longer), its texture, colour
// and depth go on as they change across it: not stretched.
void move_corners(Vertex* v, std::size_t size, const int* dx) {
  bool same = true;
  for (std::size_t i = 1; i < size; ++i) same = same && dx[i] == dx[0];
  if (!same) {
    // How each part changes with x: from the three corners that span the
    // most (as GeometryEngine's widen_across()).
    std::size_t a = 0, b = 1, c = 2;
    double best = 0;
    for (std::size_t i = 0; i < size; ++i)
      for (std::size_t j = i + 1; j < size; ++j)
        for (std::size_t k = j + 1; k < size; ++k) {
          const double area = std::abs(static_cast<double>(v[j].fine_x - v[i].fine_x) * (v[k].fine_y - v[i].fine_y) -
                                       static_cast<double>(v[k].fine_x - v[i].fine_x) * (v[j].fine_y - v[i].fine_y));
          if (area > best) {
            best = area;
            a = i, b = j, c = k;
          }
        }
    if (best > 0) {
      const double x1 = v[b].fine_x - v[a].fine_x, y1 = v[b].fine_y - v[a].fine_y;
      const double x2 = v[c].fine_x - v[a].fine_x, y2 = v[c].fine_y - v[a].fine_y;
      const double det = x1 * y2 - x2 * y1;
      // Change per DS pixel across.
      const auto per_x = [&](double pa, double pb, double pc) { return 16.0 * ((pb - pa) * y2 - (pc - pa) * y1) / det; };
      const Vertex old[3] = {v[a], v[b], v[c]};
      const double ds = per_x(old[0].s, old[1].s, old[2].s), dt = per_x(old[0].t, old[1].t, old[2].t);
      const double dz = per_x(old[0].z, old[1].z, old[2].z);
      double dcolour[3];
      for (unsigned n = 0; n < 3; ++n) {
        const auto part = [&](const Vertex& q) { return static_cast<double>((q.colour >> (6 * n)) & 0x3F); };
        dcolour[n] = per_x(part(old[0]), part(old[1]), part(old[2]));
      }
      for (std::size_t i = 0; i < size; ++i) {
        if (dx[i] == 0) continue;
        Vertex& p = v[i];
        p.s = static_cast<std::int16_t>(std::clamp<long>(std::lround(p.s + ds * dx[i]), -32768, 32767));
        p.t = static_cast<std::int16_t>(std::clamp<long>(std::lround(p.t + dt * dx[i]), -32768, 32767));
        p.z = static_cast<std::int32_t>(std::lround(p.z + dz * dx[i]));
        std::uint32_t colour = 0;
        for (unsigned n = 0; n < 3; ++n) {
          const long value = std::lround(((p.colour >> (6 * n)) & 0x3F) + dcolour[n] * dx[i]);
          colour |= static_cast<std::uint32_t>(std::clamp<long>(value, 0, 63)) << (6 * n);
        }
        p.colour = colour;
      }
    }
  }
  for (std::size_t i = 0; i < size; ++i) {
    v[i].x += dx[i];
    v[i].fine_x += dx[i] * 16;
  }
}

bool same_texture(const Polygon& a, const Polygon& b) { return a.texture == b.texture && a.palette == b.palette; }

}  // namespace

// --- HudPlan

static_assert(HudPlan::kWords == Frame2D::kHudWords);

int HudPlan::source_x(unsigned layer, int line, int x) const {
  for (unsigned i = 0; i < area_count; ++i) {
    const Area& a = areas[i];
    if ((a.layers & (1u << layer)) == 0 || line < a.y0 || line >= a.y1) continue;
    if (x >= a.x0 + a.dx && x < a.x1 + a.dx) return x - a.dx;
  }
  for (unsigned i = 0; i < area_count; ++i) {
    const Area& a = areas[i];
    if ((a.layers & (1u << layer)) == 0 || line < a.y0 || line >= a.y1) continue;
    if (x >= a.x0 && x < a.x1) return kEmpty;
  }
  return x;
}

bool HudPlan::moves(unsigned layer, int line) const {
  for (unsigned i = 0; i < area_count; ++i) {
    const Area& a = areas[i];
    if ((a.layers & (1u << layer)) != 0 && line >= a.y0 && line < a.y1) return true;
  }
  return false;
}

bool HudPlan::moves_sprites() const {
  return std::any_of(sprite_dx.begin(), sprite_dx.end(), [](std::int8_t dx) { return dx != 0; });
}

int HudPlan::touch_x(int x, int y) const {
  for (unsigned i = 0; i < touch_count; ++i) {
    const Touch& t = touches[i];
    if (x >= t.x0 && x < t.x1 && y >= t.y0 && y < t.y1) return x - t.dx;
  }
  return x;
}

void HudPlan::to_words(std::uint32_t* words) const {
  std::fill(words, words + kWords, 0u);
  if (!on) return;
  words[0] = (1u << 31) | area_count;
  for (unsigned i = 0; i < 128; ++i)
    words[1 + i / 4] |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(sprite_dx[i])) << (8 * (i % 4));
  const auto pair = [](int low, int high) {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(low)) | static_cast<std::uint32_t>(static_cast<std::uint16_t>(high)) << 16;
  };
  for (unsigned i = 0; i < area_count; ++i) {
    const Area& a = areas[i];
    words[33 + 3 * i] = pair(a.x0, a.x1);
    words[34 + 3 * i] = pair(a.y0, a.y1);
    words[35 + 3 * i] = pair(a.dx, a.layers);
  }
}

void HudPlan::add_area(int x0, int y0, int x1, int y1, int dx, unsigned layers) {
  if (area_count >= kMostAreas || x1 <= x0 || y1 <= y0 || dx == 0 || layers == 0) return;
  areas[area_count++] = Area{static_cast<std::int16_t>(x0), static_cast<std::int16_t>(y0), static_cast<std::int16_t>(x1),
                             static_cast<std::int16_t>(y1), static_cast<std::int16_t>(dx), static_cast<std::uint8_t>(layers)};
}

void HudPlan::add_touch(int x0, int y0, int x1, int y1, int dx) {
  if (touch_count >= kMostTouches || x1 <= x0 || y1 <= y0 || dx == 0) return;
  touches[touch_count++] = Touch{static_cast<std::int16_t>(x0 + dx), static_cast<std::int16_t>(y0),
                                 static_cast<std::int16_t>(x1 + dx), static_cast<std::int16_t>(y1),
                                 static_cast<std::int16_t>(dx)};
}

// --- WideHud

void WideHud::apply_3d(const Frame& frame, Frame& out) {
  out.vertices.clear();
  out.polygons.clear();
  out.swap_parameter = frame.swap_parameter;
  out.depth_is_w = frame.depth_is_w;
  found_ = {};
  flat_over_stats_ = false;
  const std::size_t n = frame.polygons.size();
  const int d = shift_;

  std::vector<Box> box(n);
  std::vector<int> owner(n, -1);
  // Each flat polygon (as the DS's view cuts it) to the first element whose
  // region it is wholly in.
  struct Region {
    Element element;
    Box box;
  };
  const Region regions[] = {{kLabel, kLabelRegion},          {kCoins, kCoinsBox.region},
                            {kPrice, kPriceBox.region},     {kItemInfo, kItemInfoBox.region},
                            {kCommandRing, kRingRegion},    {kHelpBar, kHelpRegion}};
  float log_top = 1e9f, band_top = 1e9f;
  std::size_t band_last = 0;
  unsigned fade_lines = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Polygon& p = frame.polygons[i];
    if (!p.flat || p.beyond || p.size == 0) continue;
    box[i] = box_of(frame, p);
    if (box[i].overlaps(kStatsArea)) flat_over_stats_ = true;
    if (box[i].x0 < -1 && box[i].x1 > 257 && box[i].y0 >= kLogTopLeast && box[i].y0 <= kLogTopMost) {
      log_top = std::min(log_top, box[i].y0);
      band_last = i;
      if (box[i].y1 >= kHelpBottom) band_top = std::min(band_top, box[i].y0);
      else if (box[i].y1 - box[i].y0 <= kLogFadeHeight) ++fade_lines;
    }
    for (const Region& r : regions)
      if (box[i].inside(r.box)) {
        owner[i] = static_cast<int>(r.element);
        break;
      }
  }
  std::size_t mark_first = n;
  if (band_top < 1e9f && fade_lines >= kLogFadeLines) {
    Found& f = found_[kLogBand];
    f.on = true;
    f.x0 = 0, f.x1 = 256, f.y0 = log_top, f.y1 = 192;
    Box mark = kNothing;
    for (std::size_t i = band_last + 1; i < n; ++i)
      if (!frame.polygons[i].beyond) mark.add(box_of(frame, frame.polygons[i]));
    if (band_last + 1 < n && mark.x1 - mark.x0 <= kLogMarkSize && mark.y1 - mark.y0 <= kLogMarkSize &&
        mark.inside(Box{0, log_top, 256, 192}))
      mark_first = band_last + 1;
  }
  const auto members = [&](Element e) {
    std::vector<std::size_t> list;
    for (std::size_t i = 0; i < n; ++i)
      if (owner[i] == static_cast<int>(e)) list.push_back(i);
    return list;
  };
  // Nothing else flat partly over a region (a box opening or closing): of
  // the polygons in no element's region.
  const std::vector<int> placed = owner;
  const auto clear_around = [&](const Box& region) {
    for (std::size_t i = 0; i < n; ++i) {
      const Polygon& p = frame.polygons[i];
      if (!p.flat || p.beyond || p.size == 0 || placed[i] >= 0) continue;
      if (box[i].overlaps(region) && !box[i].inside(region)) return false;
    }
    return true;
  };
  const auto drop = [&](Element e) {
    for (int& o : owner)
      if (o == static_cast<int>(e)) o = -1;
  };

  // Boxes.
  const struct {
    Element element;
    const BoxRule* rule;
  } boxes[] = {{kCoins, &kCoinsBox}, {kPrice, &kPriceBox}, {kItemInfo, &kItemInfoBox}};
  for (const auto& b : boxes) {
    const std::vector<std::size_t> list = members(b.element);
    Box all = kNothing;
    for (std::size_t i : list) all.add(box[i]);
    Box want = b.rule->shape;
    if (b.rule->any_height) want.y1 = all.y1;
    if (list.size() >= kLeastPieces && all.near(want, kShapeSlack) && clear_around(b.rule->region)) {
      Found& f = found_[b.element];
      f.on = true;
      f.x0 = all.x0, f.y0 = all.y0, f.x1 = all.x1, f.y1 = all.y1;
    } else {
      drop(b.element);
    }
  }

  // Bars: the ring of commands and the help bar. Their frame is the pieces
  // with the texture of the end at the left edge.
  const auto find_bar = [&](Element e, bool ring) -> const Polygon* {
    const std::vector<std::size_t> list = members(e);
    const Polygon* frame_piece = nullptr;
    for (std::size_t i : list) {
      const Box& b = box[i];
      const bool end = ring ? (b.x0 <= 0.5f && b.x1 - b.x0 >= kRingEndLeast && b.x1 - b.x0 <= kRingEndMost &&
                               b.y1 - b.y0 >= kRingEndHeight)
                            : (b.x0 <= 0.5f && b.x1 - b.x0 <= kHelpEndMost && b.y1 >= kHelpBottom);
      if (end) {
        frame_piece = &frame.polygons[i];
        break;
      }
    }
    if (frame_piece == nullptr) {
      drop(e);
      return nullptr;
    }
    Found& f = found_[e];
    float left = -1e9f, right = 1e9f, most = -1e9f, top = 1e9f;
    Box all = kNothing;
    for (std::size_t i : list) {
      all.add(box[i]);
      if (!same_texture(frame.polygons[i], *frame_piece)) continue;
      most = std::max(most, box[i].x1);
      top = std::min(top, box[i].y0);
      if (box[i].x0 <= 0.5f) left = std::max(left, box[i].x1);
    }
    for (std::size_t i : list)
      if (same_texture(frame.polygons[i], *frame_piece) && box[i].x1 >= most - 0.5f) right = std::min(right, box[i].x0);
    // The right end at the screen's edge (the ring), or at least past the
    // middle (the help bar, the B button beside it); a middle between.
    bool middle = false;
    for (std::size_t i : list)
      if (same_texture(frame.polygons[i], *frame_piece) && box[i].x0 <= left + 0.5f && box[i].x1 >= left + 1.0f)
        middle = true;
    const bool ends = ring ? most >= 255.5f : most >= 128.0f;
    if (!ends || !middle || right <= left || !clear_around(ring ? kRingRegion : kHelpRegion)) {
      drop(e);
      return nullptr;
    }
    f.on = true;
    f.x0 = all.x0, f.y0 = all.y0, f.x1 = all.x1, f.y1 = all.y1;
    f.cut_left = left;
    f.cut_right = right;
    f.frame_y0 = top;
    return frame_piece;
  };
  const Polygon* ring_frame = find_bar(kCommandRing, true);
  const Polygon* help_frame = find_bar(kHelpBar, false);

  // Out: each polygon as it is, moved, or made longer.
  int dx[10];
  Vertex corners[10];
  const auto put = [&](const Polygon& p, const int* moves) {
    const std::size_t size = p.size;
    std::copy(frame.vertices.begin() + p.first, frame.vertices.begin() + p.first + size, corners);
    if (moves != nullptr) move_corners(corners, size, moves);
    Polygon q = p;
    q.first = static_cast<std::uint16_t>(out.vertices.size());
    q.beyond = q.has_beyond = false;
    out.vertices.insert(out.vertices.end(), corners, corners + size);
    out.polygons.push_back(q);
  };
  const auto bar_corner = [&](const Found& f, float x) {
    return x <= f.cut_left + 0.5f ? -d : x >= f.cut_right - 0.5f ? d : 0;
  };
  for (std::size_t i = 0; i < n; ++i) {
    const Polygon& p = frame.polygons[i];
    if (p.beyond) {
      // Past the DS's view: in the longer ring, an icon (not its frame).
      if (ring_frame == nullptr || same_texture(p, *ring_frame) || !p.flat) continue;
      const bool paired = i > 0 && frame.polygons[i - 1].has_beyond;
      if (paired ? owner[i - 1] != static_cast<int>(kCommandRing) : !box_of(frame, p).inside(kRingBeyondRegion)) continue;
      put(p, nullptr);
      continue;
    }
    if (i >= mark_first) {
      std::fill(dx, dx + p.size, -d);
      put(p, dx);
      continue;
    }
    const int o = owner[i];
    if (o < 0 || d == 0) {
      put(p, nullptr);
      continue;
    }
    const std::size_t size = p.size;
    switch (static_cast<Element>(o)) {
      case kCoins:
      case kPrice:
      case kItemInfo: {
        const int dir = o == kCoins ? kCoinsBox.direction : o == kPrice ? kPriceBox.direction : kItemInfoBox.direction;
        std::fill(dx, dx + size, dir * d);
        put(p, dx);
        break;
      }
      case kCommandRing: {
        if (same_texture(p, *ring_frame)) {
          for (std::size_t k = 0; k < size; ++k)
            dx[k] = bar_corner(found_[kCommandRing], static_cast<float>(frame.vertices[p.first + k].fine_x) / 16.0f);
          put(p, dx);
        } else if (!(p.has_beyond && i + 1 < n && frame.polygons[i + 1].beyond)) {
          put(p, nullptr);  // An icon or the light on the chosen one: as it is (else its wider self, next).
        }
        break;
      }
      case kHelpBar: {
        const Found& f = found_[kHelpBar];
        if (same_texture(p, *help_frame)) {
          for (std::size_t k = 0; k < size; ++k)
            dx[k] = bar_corner(f, static_cast<float>(frame.vertices[p.first + k].fine_x) / 16.0f);
        } else {
          // The B button: with the right end; anything at the left end with it.
          const int move = box[i].x0 >= f.cut_right - 0.5f ? d : box[i].x1 <= f.cut_left + 0.5f ? -d : 0;
          std::fill(dx, dx + size, move);
        }
        put(p, dx);
        break;
      }
      default:
        put(p, nullptr);
        break;
    }
  }
}

const HudPlan& WideHud::plan_2d(const Screen2D& screen) {
  plan_.clear();
  if (!on()) return plan_;
  const int d = shift_;
  const int text = screen.text_layer;
  const unsigned text_bit = text >= 0 ? 1u << text : 0u;

  // The sprites: where each is (its room), unless switched off.
  struct Sprite {
    bool on = false;
    Box box;
  };
  std::array<Sprite, 128> sprites{};
  if (screen.sprites != nullptr && screen.sprites_on) {
    static constexpr std::uint8_t kSizes[3][4][2] = {{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
                                                     {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
                                                     {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    const auto word = [&](unsigned at) { return static_cast<unsigned>(screen.sprites[at] | screen.sprites[at + 1] << 8); };
    for (unsigned i = 0; i < 128; ++i) {
      const unsigned a0 = word(i * 8), a1 = word(i * 8 + 2);
      if (((a0 >> 8) & 3) == 2 || (a0 >> 14) == 3) continue;
      int w = kSizes[a0 >> 14][a1 >> 14][0], h = kSizes[a0 >> 14][a1 >> 14][1];
      if ((a0 & 0x300) == 0x300) w *= 2, h *= 2;
      int y = static_cast<int>(a0 & 0xFF);
      if (y >= 192) y -= 256;
      int x = static_cast<int>(a1 & 0x1FF);
      if (x >= 256) x -= 512;
      sprites[i].on = true;
      sprites[i].box = Box{static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h)};
    }
  }
  // Moves the sprites in `region` by dx. With `whole` (an icon), those
  // wholly in it, and only if none is partly in it; else (words in a box,
  // drawn from the top left of sprites that can reach past the box) those
  // whose top left corner is in it. Whether any moved.
  const auto in_region = [](const Box& b, const Box& region, bool whole) {
    return whole ? b.inside(region, 0)
                 : b.x0 >= region.x0 && b.x0 < region.x1 && b.y0 >= region.y0 && b.y0 < region.y1;
  };
  const auto move_sprites = [&](const Box& region, int dx, bool whole) {
    bool any = false;
    for (const Sprite& s : sprites) {
      if (!s.on) continue;
      if (in_region(s.box, region, whole)) any = true;
      else if (whole && s.box.overlaps(region, 0)) return false;
    }
    if (!any) return false;
    Box moved = kNothing;
    for (unsigned i = 0; i < 128; ++i)
      if (sprites[i].on && in_region(sprites[i].box, region, whole)) {
        plan_.sprite_dx[i] = static_cast<std::int8_t>(dx);
        moved.add(sprites[i].box);
      }
    plan_.add_touch(static_cast<int>(moved.x0), static_cast<int>(moved.y0), static_cast<int>(moved.x1),
                    static_cast<int>(moved.y1), dx);
    return true;
  };
  const auto has = [&](const Box& b) {
    return text >= 0 && screen.has_pixels &&
           screen.has_pixels(static_cast<unsigned>(text), static_cast<int>(b.x0), static_cast<int>(b.y0),
                             static_cast<int>(b.x1), static_cast<int>(b.y1));
  };

  bool menus = false;
  // The boxes: their text and sprites with them.
  const struct {
    Element element;
    const BoxRule* rule;
  } boxes[] = {{kCoins, &kCoinsBox}, {kPrice, &kPriceBox}, {kItemInfo, &kItemInfoBox}};
  for (const auto& b : boxes) {
    const Found& f = found_[b.element];
    if (!f.on) continue;
    menus = true;
    const int dx = b.rule->direction * d;
    const Box& r = b.rule->region;
    plan_.add_area(static_cast<int>(r.x0), static_cast<int>(r.y0), static_cast<int>(r.x1), static_cast<int>(r.y1), dx, text_bit);
    move_sprites(r, dx, false);
    plan_.add_touch(static_cast<int>(f.x0), static_cast<int>(f.y0), static_cast<int>(std::ceil(f.x1)),
                    static_cast<int>(std::ceil(f.y1)), dx);
  }
  // The help bar: its text to the left, what is at its right end (the B
  // button) to the right.
  if (const Found& f = found_[kHelpBar]; f.on) {
    menus = true;
    const int cut = static_cast<int>(f.cut_right), top = static_cast<int>(f.frame_y0);
    plan_.add_area(0, top, cut, 192, -d, text_bit);
    plan_.add_area(cut, top, 256, 192, d, text_bit);
    const Box left{-8, f.y0, static_cast<float>(cut), 200}, right{static_cast<float>(cut), f.y0, 264, 200};
    move_sprites(left, -d, false);
    move_sprites(right, d, false);
    plan_.add_touch(cut, static_cast<int>(f.y0), 256, 192, d);
  }
  if (found_[kCommandRing].on) menus = true;
  // A battle's message: to the left side.
  if (const Found& f = found_[kLogBand]; f.on) {
    menus = true;
    plan_.add_area(0, static_cast<int>(f.y0), 256, 192, -d, text_bit);
    // The mark that the message goes on, with it.
    move_sprites(Box{-64, f.y0, 256, 192}, -d, false);
  }
  // The party's numbers: to the right side.
  if (!flat_over_stats_ && has(kStatsArea) && !has(kStatsLeftOf) && !has(kStatsUnder))
    plan_.add_area(static_cast<int>(kStatsArea.x0), static_cast<int>(kStatsArea.y0), static_cast<int>(kStatsArea.x1),
                   static_cast<int>(kStatsArea.y1), d, text_bit);
  // The field's shortcuts.
  if (!menus)
    for (const SpriteRule& rule : kFieldShortcuts) move_sprites(rule.region, rule.direction * d, true);

  plan_.on = plan_.area_count != 0 || plan_.moves_sprites();
  return plan_;
}

std::string WideHud::found() const {
  static const char* const kNames[kElements] = {"coins", "price", "item info", "command ring", "help bar", "battle message", "label"};
  std::string text;
  for (unsigned e = 0; e < kElements; ++e)
    if (found_[e].on) text += (text.empty() ? "" : ", ") + std::string(kNames[e]);
  return text;
}

}  // namespace bd::nds
