// The launcher: what the window program shows when it is started without a
// ROM, so that a player never needs the command line.
//
// Until the game has been built for this version of Bright Dawn, it is the
// setup instead: one page, on which the player chooses their ROM and builds
// the game, and only then goes on to the launcher. Once the ROM is chosen,
// both are dressed in the game's own menus, read from that ROM: the settings
// menu's wooden board, its title ribbon and message bar, the travel journal's
// parchment, and the game's font (app/menu_skin.h).
//
// The launcher's pages:
//   Play      choose the ROM (remembered), and play it;
//   Build     build the game library from the ROM, its code translated to
//             C++ and compiled for this computer, following the builder as
//             it goes; what it needs and the computer lacks is fetched for
//             the build and removed after it;
//   Settings  the settings book;
//   Log       where this run's log is.
#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app/app.h"
#include "app/build_tools.h"
#include "app/child_process.h"
#include "app/fetch.h"
#include "app/menu_skin.h"
#include "app/menu_widgets.h"
#include "app/settings_book.h"
#include "rom/rom.h"
#include "ui/build_progress.h"

namespace bd::app {

struct LauncherChoice {
  enum Kind { Quit, Play } kind = Quit;
  std::filesystem::path rom;
};

class Launcher {
 public:
  explicit Launcher(App& app);
  ~Launcher();
  // Shows the launcher until the player chooses.
  LauncherChoice run();

 private:
  enum class Page { Play, Build, Settings, Log };
  struct RomCheck {
    bool ok = false;
    std::string text;  // What it is, or what is wrong with it.
  };
  enum class BuiltState { None, Ready, OtherVersion, Broken };

  // Between frames only: the game's menus, read from the ROM, change.
  void choose_rom(const std::filesystem::path& path);
  // The game's menus from the ROM, now open, for the launcher's look.
  void read_menu_art(const rom::Rom& rom);
  // In the game's look when its menus could be read from the ROM; null for
  // ImGui's own.
  const menu::Look* look() const { return skin_.ready() && app_.gui.has_game_font() ? &look_ : nullptr; }
  // Whether the setup has to be gone through first: the game is not built
  // for this version (the build of the developers' own, with the game linked
  // in, needs none).
  bool needs_setup() const;
  void check_tools();
  void check_built_game();
  bool building() const { return fetch_ != nullptr || build_ != nullptr; }
  void begin_build_output();
  void add_output(std::string_view text);
  void start_build();
  void start_builder(const std::vector<std::string>& python);
  void stop_build();
  void finish_build(bool ok, const std::string& result);
  void poll_build();
  void play(LauncherChoice& choice);

  void draw(LauncherChoice& choice, bool& done);
  void draw_setup(LauncherChoice& choice, bool& done);
  void draw_setup_page();
  // In the game's look: the title on its ribbon, the pages on the wooden
  // board, and the message bar below it.
  void draw_dressed(LauncherChoice& choice, bool& done);
  void draw_pages_dressed(LauncherChoice& choice, bool& done);
  void draw_setup_dressed(LauncherChoice& choice, bool& done);
  // A page on parchment, filling what is left of the current window.
  void begin_paper(const char* id);
  void end_paper();
  void draw_rom_chooser();
  void draw_play(LauncherChoice& choice, bool& done);
  void draw_build();
  void draw_log();

  // Pieces of the pages, in either look.
  void heading(const char* text) const;
  void note(const std::string& text) const;
  void coloured(bool good, const std::string& text) const;
  ImVec4 colour(bool good) const;
  bool button(const char* label, ImVec2 size = ImVec2(0, 0)) const;
  bool small_button(const char* label) const;
  // What the item just drawn does, for the message bar when it is hovered.
  void explain(const char* text);
  // The message bar's text when nothing is hovered.
  std::string standing_hint() const;

  App& app_;
  Page page_ = Page::Play;
  bool setup_ = false;  // The setup is shown instead of the launcher.
  MenuSkin skin_;
  menu::Look look_;
  bool skin_changed_ = false;  // The game font is to change before the next frame.
  std::string hint_;           // For the message bar, this frame.
  SettingsBook book_;
  std::filesystem::path rom_;
  // Typed by the player during a frame: chosen before the next.
  std::optional<std::filesystem::path> rom_asked_;
  RomCheck rom_check_;
  std::string rom_typed_;
  // The file dialog answers from another thread.
  struct DialogAnswer {
    std::mutex mutex;
    std::optional<std::string> path;
    std::string error;
    bool open = false;
  };
  std::shared_ptr<DialogAnswer> dialog_ = std::make_shared<DialogAnswer>();

  std::filesystem::path library_;  // Where the game library is kept.
  std::filesystem::path program_;  // This program, which the library is built for.
  std::optional<std::filesystem::path> builder_;
  FoundTool python_;
  bool tools_checked_ = false;
  BuiltState built_state_ = BuiltState::None;
  std::string built_detail_;

  std::unique_ptr<Fetch> fetch_;
  std::unique_ptr<ChildProcess> build_;
  ui::BuildProgress progress_;
  std::deque<std::string> output_;
  bool scroll_to_end_ = false;
  bool stopping_ = false;
  std::uint64_t stop_asked_at_ = 0;
  std::uint64_t build_started_at_ = 0;
  std::string build_result_;
  bool build_failed_ = false;
};

}  // namespace bd::app
