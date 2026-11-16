// The game's fonts, as the DS keeps them (Nintendo's NFTR format): a header
// (FINF), the glyphs as small bitmaps of one or more bits a pixel (CGLP),
// each glyph's widths (CWDH), and which character is which glyph (CMAP).
// The files in the game are LZ40 compressed. Only the shapes are in the
// file; the game adds the outline when it draws text, and so does the
// launcher (see app/game_font.h).
#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/bytes.h"
#include "core/result.h"
#include "rom/rom.h"

namespace bd::assets {

struct Glyph {
  int left = 0;     // Pixels left blank before the bitmap.
  int width = 0;    // Of the bitmap's columns that are used.
  int advance = 0;  // Where the next character starts.
  // cell_width by cell_height values, row after row: 0 is blank, the
  // largest value (1 for one bit a pixel) full ink.
  std::vector<std::uint8_t> pixels;
};

class Font {
 public:
  // Reads a font file, compressed or not. Fails when it is not one, or a
  // part of it lies outside the file.
  static Result<Font> parse(ByteSpan file);

  int cell_width() const { return cell_width_; }
  int cell_height() const { return cell_height_; }
  int line_height() const { return line_height_; }
  int baseline() const { return baseline_; }        // Rows from the cell's top.
  int max_value() const { return (1 << bits_) - 1; }  // Full ink.
  std::size_t glyph_count() const { return glyphs_.size(); }

  // The glyph for `code` (Unicode), or null when the font has none.
  const Glyph* find(char32_t code) const;
  // Every character the font has.
  std::vector<char32_t> characters() const;

 private:
  int cell_width_ = 0;
  int cell_height_ = 0;
  int line_height_ = 0;
  int baseline_ = 0;
  int bits_ = 1;
  std::vector<Glyph> glyphs_;
  std::unordered_map<char32_t, std::uint32_t> map_;
};

// The font at `path` in the ROM's file system.
Result<Font> read_font(const rom::Rom& rom, std::string_view path);

}  // namespace bd::assets
