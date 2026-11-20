// The look of the game's own menus for the launcher, made of what the
// player's ROM holds (nothing of the game comes with Bright Dawn): the
// settings menu's background, a wooden board on blue-green with gears; its
// strips of parchment for each line; the ribbon its title sits on; the
// travel journal's parchment for pages; the page of the list of saves, for
// the launcher's first page; and the game's font. Buttons and the
// message bar are drawn in the menus' colours.
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "assets/font.h"
#include "assets/screen_picture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "rom/rom.h"

namespace bd::app {

// Where each piece is in the ROM, and the parts of the pictures that are used.
namespace skin {
inline constexpr std::string_view kBackground = "data/config/bg.bin";  // 256 by 192
inline constexpr std::string_view kStrip = "data/config/frame_cv.bin";   // 232 by 32
inline constexpr std::string_view kRibbonLeft = "data/config/title1_l.bin";    // 40 by 20
inline constexpr std::string_view kRibbonMiddle = "data/config/title1_c.bin";  // 16 by 20
inline constexpr std::string_view kRibbonRight = "data/config/title1_r.bin";   // 40 by 20
inline constexpr std::string_view kJournal = "data/upscreen/backupStory.bin";  // 256 by 192
inline constexpr std::string_view kSavePage = "data/nameentry/down_menu.bin";  // 256 by 192
inline constexpr std::string_view kFont = "data/font/LC1212.dat";
// In the background: the wooden board; the sky above it, between the gears,
// where the menu's title is; and below it, where its message bar is.
inline constexpr int kBoardLeft = 9, kBoardTop = 30, kBoardRight = 246, kBoardBottom = 162;
inline constexpr float kTitleY = 14.0f;
inline constexpr int kMessageLeft = 10, kMessageTop = 166, kMessageRight = 246, kMessageBottom = 190;
// In the journal: parchment with nothing drawn on it.
inline constexpr int kPaperLeft = 32, kPaperTop = 30, kPaperRight = 104, kPaperBottom = 120;
// On the page of the game's list of saves (an open book with a gold strip
// for its buttons): its first two lines, the strip, and the parchment below.
inline constexpr int kSaveLineLeft = 16, kSaveLineRight = 250;
inline constexpr int kSaveFirstTop = 25, kSaveFirstBottom = 59, kSaveSecondBottom = 97, kSaveThirdBottom = 135;
inline constexpr int kSaveStripTop = 141, kSaveStripBottom = 166;
inline constexpr int kSaveBelowTop = 168, kSaveBelowBottom = 190;
}  // namespace skin

// A picture as a texture, enlarged to a whole multiple of its size once and
// drawn with linear filtering: sharp pixels, smooth edges.
class SkinPicture {
 public:
  SkinPicture() = default;
  SkinPicture(const SkinPicture&) = delete;
  SkinPicture& operator=(const SkinPicture&) = delete;
  ~SkinPicture() { release(); }
  void set(assets::Picture picture);
  void clear();
  bool shown() const { return picture_.has_value(); }
  int width() const { return picture_ ? picture_->width : 0; }
  int height() const { return picture_ ? picture_->height : 0; }
  // The texture, made on first use; null when it cannot be.
  SDL_Texture* texture(SDL_Renderer* renderer);
  void release();

 private:
  std::optional<assets::Picture> picture_;
  SDL_Texture* texture_ = nullptr;
  bool failed_ = false;
};

class MenuSkin {
 public:
  // Reads everything from the ROM. What is missing is logged; the launcher
  // then looks as it does without a ROM.
  void read(const rom::Rom& rom);
  void clear();
  bool ready() const { return ready_; }
  std::shared_ptr<const assets::Font> font() const { return font_; }
  // Before the renderer goes.
  void release();

  // The background, filling the renderer's output. Where the window's shape
  // is not the picture's, only parts with nothing but wood and sky in them
  // are stretched: the gears keep their shape, and nothing is cut off.
  void draw_background(SDL_Renderer* renderer) { draw_whole(renderer, background_); }
  // The page of the list of saves instead, placed the same way. (The
  // launcher's window has the pictures' shape, so neither is stretched.)
  void draw_save_page(SDL_Renderer* renderer) { draw_whole(renderer, save_page_); }
  // Where a point of the background picture (in its pixels) is in the
  // window, in ImGui's coordinates; and a rectangle of it.
  ImVec2 at(SDL_Renderer* renderer, float x, float y) const;
  ImRect area(SDL_Renderer* renderer, float left, float top, float right, float bottom) const;
  // How large one of the picture's pixels is, in ImGui's units, where it is
  // not stretched.
  float picture_scale(SDL_Renderer* renderer) const;
  // Where the wooden board is in the window, in ImGui's coordinates.
  ImRect board(SDL_Renderer* renderer) const;
  // Pieces drawn with ImGui, into `list`.
  void draw_strip(ImDrawList* list, SDL_Renderer* renderer, ImVec2 min, ImVec2 max);
  void draw_paper(ImDrawList* list, SDL_Renderer* renderer, ImVec2 min, ImVec2 max);
  // The ribbon, `height` tall and centred on `centre`, long enough for a
  // title `text_width` wide; returns where the title goes.
  ImRect draw_ribbon(ImDrawList* list, SDL_Renderer* renderer, ImVec2 centre, float height, float text_width);

 private:
  // The background's fit to the renderer's output, in its pixels: the scale
  // at which the whole picture fits, and the width or height left over,
  // which the stretched parts take up.
  struct Fit {
    float scale = 0;
    float extra_width = 0;
    float extra_height = 0;
  };
  bool fit(SDL_Renderer* renderer, Fit& fit) const;
  // A point of the picture in the renderer's pixels.
  static ImVec2 place(const Fit& fit, float x, float y);
  void draw_whole(SDL_Renderer* renderer, SkinPicture& picture);

  bool ready_ = false;
  SkinPicture background_;
  SkinPicture strip_;
  SkinPicture ribbon_left_;
  SkinPicture ribbon_middle_;
  SkinPicture ribbon_right_;
  SkinPicture paper_;
  SkinPicture save_page_;
  std::shared_ptr<const assets::Font> font_;
};

// The menus' colours.
namespace skin {
inline const ImVec4 kText(1.00f, 0.98f, 0.92f, 1.0f);         // Light, outlined in black.
inline const ImVec4 kInk(0.27f, 0.16f, 0.08f, 1.0f);           // Dark brown, on parchment.
inline const ImVec4 kButton(0.62f, 0.62f, 0.64f, 1.0f);        // A choice not taken: grey,
inline const ImVec4 kButtonText(0.40f, 0.42f, 0.48f, 1.0f);    // with grey text.
inline const ImVec4 kChosen(0.99f, 0.89f, 0.35f, 1.0f);        // The choice taken: yellow,
inline const ImVec4 kChosenText(0.52f, 0.22f, 0.04f, 1.0f);    // with brown text.
inline const ImVec4 kEdge(0.10f, 0.13f, 0.31f, 1.0f);          // Buttons' edges: dark blue.
inline const ImVec4 kAction(0.22f, 0.53f, 0.85f, 1.0f);        // A button that does something: blue.
inline const ImVec4 kMessage(0.19f, 0.42f, 0.62f, 0.96f);      // The message bar,
inline const ImVec4 kMessageEdge(0.56f, 0.38f, 0.16f, 1.0f);   // in a frame of brown.
}  // namespace skin

}  // namespace bd::app
