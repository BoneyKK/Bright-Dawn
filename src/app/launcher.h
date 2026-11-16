// The launcher: what the window program shows when it is started without a
// ROM, so that a player never needs the command line. Its pages:
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
