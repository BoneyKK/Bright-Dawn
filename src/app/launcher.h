// The launcher: what the window program shows when it is started without a
// ROM, so that a player never needs the command line. Its pages:
//   Play      choose the ROM (remembered), and play it;
//   Build     build the game from the ROM, its code translated to C++ and
//             compiled for this computer, following the builder as it goes;
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
#include "app/settings_book.h"
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

  void choose_rom(const std::filesystem::path& path);
  void check_tools();
  void check_built_game();
  void start_build();
  void stop_build();
  void poll_build();
  void play(LauncherChoice& choice);

  void draw(LauncherChoice& choice, bool& done);
  void draw_play(LauncherChoice& choice, bool& done);
  void draw_build();
  void draw_log();

  App& app_;
  Page page_ = Page::Play;
  SettingsBook book_;
  std::filesystem::path rom_;
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

  std::filesystem::path work_;
  std::optional<std::filesystem::path> builder_;
  FoundTool python_, cmake_;
  bool tools_checked_ = false;
  std::optional<std::filesystem::path> built_;
  BuiltState built_state_ = BuiltState::None;
  std::string built_version_;

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
