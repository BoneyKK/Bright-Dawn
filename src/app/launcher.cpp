#include "app/launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/version.h"
#include "imgui.h"
#include "rom/game_id.h"
#include "rom/rom.h"
#include "ui/game_settings.h"

namespace bd::app {
namespace {

constexpr std::size_t kMostOutputLines = 5000;

const ImVec4 kGood(0.55f, 0.82f, 0.45f, 1.0f);
const ImVec4 kBad(0.93f, 0.45f, 0.38f, 1.0f);

void heading(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void note(const std::string& text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

void coloured(const ImVec4& colour, const std::string& text) {
  ImGui::PushStyleColor(ImGuiCol_Text, colour);
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

void open_folder(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::string url = "file:///" + utf8(folder);
  for (char& c : url)
    if (c == '\\') c = '/';
  // "file:////home" is read as a network share on some systems.
  if (url.rfind("file:////", 0) == 0) url.erase(7, 1);
  if (!SDL_OpenURL(url.c_str())) log::warning("launcher", std::string("cannot open the folder: ") + SDL_GetError());
}

}  // namespace

Launcher::Launcher(App& app) : app_(app) {
  if (!app_.user_data.empty()) work_ = app_.user_data / "game-build";
  const std::string remembered = app_.settings.value(ui::key::kRom);
  if (!remembered.empty()) choose_rom(from_utf8(remembered));
  builder_ = find_builder();
  check_built_game();
}

Launcher::~Launcher() {
  if (build_ == nullptr || !build_->running()) return;
  // The window is closing during a build: the builder is asked to stop, and
  // made to if it does not.
  log::info("builder", "stopping the build: the program is closing");
  build_->close_input();
  for (int i = 0; i < 1500 && build_->running(); ++i) {
    build_->poll([](std::string_view) {});
    SDL_Delay(10);
  }
  if (build_->running()) build_->kill();
}

void Launcher::choose_rom(const std::filesystem::path& path) {
  rom_ = path;
  rom_check_ = {};
  rom_typed_ = utf8(path);
  Result<rom::Rom> rom = rom::Rom::open(path);
  if (!rom) {
    rom_check_.text = rom.error().message;
  } else if (Status status = rom::require_dark_dawn(rom.value().header()); !status) {
    rom_check_.text = status.error().message;
  } else {
    const rom::KnownTitle* title = rom::identify_title(rom.value().header());
    rom_check_.ok = true;
    rom_check_.text = std::string("Golden Sun: Dark Dawn (") + rom::to_string(title->region) + ", " +
                      std::string(title->game_code) + ")";
  }
  log::writef(rom_check_.ok ? log::Level::Info : log::Level::Warning, "launcher", "ROM %s: %s", utf8(path).c_str(),
              rom_check_.text.c_str());
  if (rom_check_.ok && app_.settings.value(ui::key::kRom) != utf8(path)) app_.set_setting(ui::key::kRom, utf8(path));
}

void Launcher::check_tools() {
  python_ = find_python();
  cmake_ = find_cmake();
  builder_ = find_builder();
  tools_checked_ = true;
  log::info("builder", "builder files: " + (builder_ ? utf8(*builder_) : std::string("not found")));
  log::info("builder", python_.found ? "Python " + python_.version + " (" + python_.command[0] + ")" : python_.problem);
  log::info("builder", cmake_.found ? "CMake " + cmake_.version : cmake_.problem);
}

void Launcher::check_built_game() {
  built_.reset();
  built_state_ = BuiltState::None;
  built_version_.clear();
  if (app_.translated_build || work_.empty()) return;
  built_ = built_game(work_);
  if (!built_) return;
  const BuiltGameVersion v = built_game_version(*built_);
  if (!v.known) {
    built_state_ = BuiltState::Broken;
    built_version_ = v.problem;
  } else {
    built_version_ = v.version;
    built_state_ = v.version == own_version_line() ? BuiltState::Ready : BuiltState::OtherVersion;
  }
  log::info("launcher", "built game " + utf8(*built_) + ": " + built_version_ +
                            (built_state_ == BuiltState::Ready ? " (this version)" : ""));
}

void Launcher::start_build() {
  if (build_ != nullptr && build_->running()) return;
  std::vector<std::string> args = python_.command;
  args.push_back(utf8(*builder_ / "tools" / "gamebuilder" / "build_game.py"));
  args.push_back(utf8(rom_));
  args.push_back("--work");
  args.push_back(utf8(work_));
  args.push_back("--stop-on-stdin-eof");
  // The game is to name the same version as this program.
  args.push_back(std::string("--cmake-arg=-DBD_BUILD_ID=") + BD_BUILD_ID);
  if (std::strlen(BD_COMMIT) != 0) args.push_back(std::string("--cmake-arg=-DBD_COMMIT=") + BD_COMMIT);
  const long long jobs = app_.settings.number(ui::key::kBuildJobs);
  if (jobs > 0) {
    args.push_back("--jobs");
    args.push_back(std::to_string(jobs));
  }
  std::string line;
  for (const std::string& a : args) line += (line.empty() ? "" : " ") + a;
  log::info("builder", "starting: " + line);
  output_.clear();
  progress_ = ui::BuildProgress();
  build_result_.clear();
  build_failed_ = false;
  stopping_ = false;
  build_ = std::make_unique<ChildProcess>();
  std::string error;
  if (!build_->start(args, true, error)) {
    build_result_ = "The builder could not be started: " + error;
    build_failed_ = true;
    log::error("builder", build_result_);
    build_.reset();
    return;
  }
  build_started_at_ = SDL_GetTicks();
}

void Launcher::stop_build() {
  if (build_ == nullptr || !build_->running() || stopping_) return;
  log::info("builder", "stopping the build, as asked");
  stopping_ = true;
  stop_asked_at_ = SDL_GetTicks();
  build_->close_input();
}

void Launcher::poll_build() {
  if (build_ == nullptr) return;
  build_->poll([&](std::string_view text) {
    progress_.take_line(text);
    if (text.rfind("== ", 0) == 0 || text.rfind("error: ", 0) == 0 || text.rfind("The game: ", 0) == 0)
      log::info("builder", std::string(text));
    output_.emplace_back(text);
    if (output_.size() > kMostOutputLines) output_.pop_front();
    scroll_to_end_ = true;
  });
  if (stopping_ && build_->running() && SDL_GetTicks() - stop_asked_at_ > 20000) {
    log::warning("builder", "the builder did not stop when asked: it is ended");
    build_->kill();
  }
  if (!build_->ended()) return;
  const std::uint64_t seconds = (SDL_GetTicks() - build_started_at_) / 1000;
  const bool ok = (!ChildProcess::kExitCodeKnown || build_->exit_code() == 0) && !progress_.game().empty();
  char took[64];
  std::snprintf(took, sizeof took, " (%" PRIu64 " min %" PRIu64 " s)", seconds / 60, seconds % 60);
  if (stopping_) {
    build_result_ = "The build was stopped.";
    build_failed_ = true;
  } else if (ok) {
    build_result_ = std::string("The game was built") + took + ".";
    build_failed_ = false;
  } else {
    build_result_ = "The build failed" + std::string(took) + ": " +
                    (progress_.error().empty() ? "exit status " + std::to_string(build_->exit_code())
                                               : progress_.error()) +
                    ".";
    build_failed_ = true;
  }
  (build_failed_ ? log::warning : log::info)("builder", build_result_);
  build_.reset();
  stopping_ = false;
  check_built_game();
}

void Launcher::play(LauncherChoice& choice) {
  const bool built = !app_.translated_build && built_state_ == BuiltState::Ready && app_.settings.toggle(ui::key::kPlayBuilt);
  if (built) {
    std::string error;
    log::info("launcher", "starting the built game " + utf8(*built_) + " with " + utf8(rom_));
    if (start_detached({utf8(*built_), utf8(rom_)}, error)) {
      choice.kind = LauncherChoice::Quit;
      return;
    }
    log::error("launcher", "the built game could not be started: " + error + "; playing interpreted instead");
  }
  choice.kind = LauncherChoice::Play;
  choice.rom = rom_;
}

LauncherChoice Launcher::run() {
  LauncherChoice choice;
  SDL_SetWindowTitle(app_.window, (std::string("Bright Dawn ") + BD_VERSION).c_str());
  const bool vsync = SDL_SetRenderVSync(app_.renderer, 1);
  log::info("launcher", "shown");
  bool done = false;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      app_.gui.process_event(event);
      if (event.type == SDL_EVENT_QUIT) {
        log::info("window", "closed");
        choice.kind = LauncherChoice::Quit;
        done = true;
      }
    }
    std::optional<std::string> chosen;
    std::string dialog_error;
    {
      std::lock_guard lock(dialog_->mutex);
      chosen.swap(dialog_->path);
      dialog_error.swap(dialog_->error);
    }
    if (chosen) choose_rom(from_utf8(*chosen));
    if (!dialog_error.empty()) log::warning("launcher", "the file dialog: " + dialog_error);
    poll_build();
    if (done) break;
    SDL_SetRenderDrawColor(app_.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app_.renderer);
    app_.gui.begin_frame();
    draw(choice, done);
    app_.gui.end_frame();
    SDL_RenderPresent(app_.renderer);
    if (!vsync) SDL_Delay(15);
  }
  return choice;
}

void Launcher::draw(LauncherChoice& choice, bool& done) {
  const ImGuiViewport* view = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(view->Pos);
  ImGui::SetNextWindowSize(view->Size);
  ImGui::Begin("Bright Dawn", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::TextUnformatted("Bright Dawn");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s%s", BD_VERSION, app_.translated_build ? ", the built game" : "");
  ImGui::Separator();

  const float index_width = ImGui::GetFontSize() * 11;
  ImGui::BeginChild("pages", ImVec2(index_width, 0), ImGuiChildFlags_Borders);
  const struct {
    Page page;
    const char* name;
  } pages[] = {{Page::Play, "Play"}, {Page::Build, "Build the game"}, {Page::Settings, "Settings"}, {Page::Log, "Log"}};
  for (const auto& p : pages) {
    if (p.page == Page::Build && app_.translated_build) continue;
    if (ImGui::Selectable(p.name, page_ == p.page)) {
      page_ = p.page;
      if (page_ == Page::Build && !tools_checked_) check_tools();
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("page", ImVec2(0, 0), page_ == Page::Settings ? ImGuiChildFlags_None : ImGuiChildFlags_Borders);
  switch (page_) {
    case Page::Play: draw_play(choice, done); break;
    case Page::Build: draw_build(); break;
    case Page::Settings: app_.settings_changed(book_.draw(app_.settings)); break;
    case Page::Log: draw_log(); break;
  }
  ImGui::EndChild();
  ImGui::End();
}

void Launcher::draw_play(LauncherChoice& choice, bool& done) {
  heading("Your ROM");
  if (rom_.empty())
    note("Choose your own copy of Golden Sun: Dark Dawn, a .nds file. Bright Dawn remembers it.");
  else
    ImGui::TextWrapped("%s", utf8(rom_).c_str());
  bool dialog_open = false;
  {
    std::lock_guard lock(dialog_->mutex);
    dialog_open = dialog_->open;
  }
  ImGui::BeginDisabled(dialog_open);
  if (ImGui::Button("Choose your ROM...")) {
    static const SDL_DialogFileFilter filters[] = {{"Nintendo DS ROM", "nds"}, {"All files", "*"}};
    std::string folder = rom_.empty() ? std::string() : utf8(rom_.parent_path());
    // The dialog may answer on another thread, after this launcher is gone:
    // it holds its own share of where the answer goes.
    auto* holder = new std::shared_ptr<DialogAnswer>(dialog_);
    {
      std::lock_guard lock(dialog_->mutex);
      dialog_->open = true;
    }
    SDL_ShowOpenFileDialog(
        [](void* userdata, const char* const* files, int) {
          auto* h = static_cast<std::shared_ptr<DialogAnswer>*>(userdata);
          {
            std::lock_guard lock((*h)->mutex);
            (*h)->open = false;
            if (files == nullptr) (*h)->error = SDL_GetError();
            else if (files[0] != nullptr) (*h)->path = files[0];
          }
          delete h;
        },
        holder, app_.window, filters, 2, folder.empty() ? nullptr : folder.c_str(), false);
  }
  ImGui::EndDisabled();
  // Where no file dialog is to be had, or for pasting a path.
  char buffer[2048];
  std::snprintf(buffer, sizeof buffer, "%s", rom_typed_.c_str());
  ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
  if (ImGui::InputTextWithHint("##path", "or type or paste its path here", buffer, sizeof buffer,
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
    rom_typed_ = buffer;
    choose_rom(from_utf8(rom_typed_));
  } else {
    rom_typed_ = buffer;
  }
  ImGui::SameLine();
  if (ImGui::Button("Use this")) choose_rom(from_utf8(rom_typed_));
  if (!rom_.empty()) coloured(rom_check_.ok ? kGood : kBad, rom_check_.text);

  ImGui::Spacing();
  heading("Play");
  if (app_.translated_build) {
    note("This is the built game: its code runs natively.");
  } else if (built_state_ == BuiltState::Ready && app_.settings.toggle(ui::key::kPlayBuilt)) {
    note("The built game is played: its code runs natively.");
  } else if (built_state_ == BuiltState::Ready) {
    note("The game is interpreted: playing the built game is turned off (Settings, Game).");
  } else {
    note("The game is interpreted, which is slow: about a third of the console's speed. Build the game "
         "(Build the game, on the left) to run its code natively.");
    if (built_state_ == BuiltState::OtherVersion)
      note("The game built before is of another version of Bright Dawn (" + built_version_ + "): build it again.");
    else if (built_state_ == BuiltState::Broken)
      note("The game built before cannot be used: " + built_version_ + ". Build it again.");
  }
  ImGui::BeginDisabled(!rom_check_.ok);
  if (ImGui::Button("Play", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f))) {
    play(choice);
    done = true;
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  note("Keyboard: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Right Shift = Select. "
       "Controllers: by position, as on the DS. The left mouse button on the touch screen is the stylus. "
       "Escape (or a controller's guide button) pauses and opens the menu. F11: full screen.");
}

void Launcher::draw_build() {
  heading("Build the game");
  note("The builder translates the game's code, from your ROM, to C++ and compiles it for this computer. "
       "It then runs natively instead of being interpreted. The translated code is made from your ROM and "
       "stays on this computer. The first build takes a while: some minutes on a fast computer, longer on "
       "a slower one, and SDL is downloaded for it.");
  ImGui::Spacing();
  ImGui::TextUnformatted("What it needs:");
  const auto requirement = [](bool ok, const std::string& what, const std::string& detail) {
    ImGui::Bullet();
    ImGui::PushStyleColor(ImGuiCol_Text, ok ? kGood : kBad);
    ImGui::TextUnformatted(what.c_str());
    ImGui::PopStyleColor();
    if (!detail.empty()) {
      ImGui::Indent();
      note(detail);
      ImGui::Unindent();
    }
  };
  requirement(rom_check_.ok, rom_check_.ok ? "Your ROM" : "Your ROM: choose it on the Play page", "");
  requirement(builder_.has_value(), builder_ ? "The builder's files" : "The builder's files were not found",
              builder_ ? utf8(*builder_)
                       : "They come with Bright Dawn, in the folder \"builder\" next to the program.");
  requirement(python_.found, python_.found ? "Python " + python_.version : python_.problem,
              python_.found ? "" : "Install Python 3.10 or later (python.org), then check again.");
  requirement(cmake_.found, cmake_.found ? "CMake " + cmake_.version : cmake_.problem,
              cmake_.found ? "" : "Install CMake 3.20 or later (cmake.org), then check again.");
#if defined(_WIN32)
  const char* compiler = "Visual Studio, or its Build Tools, with \"Desktop development with C++\".";
#elif defined(__APPLE__)
  const char* compiler = "Xcode's command line tools (xcode-select --install).";
#else
  const char* compiler =
      "g++ or clang++, and the development files of X11 or Wayland that SDL needs (the build names any that are missing).";
#endif
  ImGui::Bullet();
  ImGui::TextWrapped("A C++ compiler: %s It is checked when the build starts.", compiler);
  const bool running = build_ != nullptr && build_->running();
  ImGui::BeginDisabled(running);
  if (ImGui::Button("Check again")) check_tools();
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::TextUnformatted("Where it builds:");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", work_.empty() ? "(no user data folder)" : utf8(work_).c_str());
  if (!work_.empty() && ImGui::SmallButton("Open the folder")) open_folder(work_);
  if (built_state_ == BuiltState::Ready) coloured(kGood, "Built, and ready: " + built_version_);
  else if (built_state_ == BuiltState::OtherVersion) coloured(kBad, "Built by another version: " + built_version_);
  else if (built_state_ == BuiltState::Broken) coloured(kBad, "Built, but it cannot be used: " + built_version_);

  ImGui::Spacing();
  const bool can_build = rom_check_.ok && builder_ && python_.found && cmake_.found && !work_.empty();
  if (!running) {
    ImGui::BeginDisabled(!can_build);
    if (ImGui::Button(built_ ? "Build it again" : "Build the game",
                      ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f)))
      start_build();
    ImGui::EndDisabled();
  } else {
    ImGui::BeginDisabled(stopping_);
    if (ImGui::Button(stopping_ ? "Stopping..." : "Stop", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f)))
      stop_build();
    ImGui::EndDisabled();
    char label[160];
    if (progress_.steps() == 0)
      std::snprintf(label, sizeof label, "Starting");
    else
      std::snprintf(label, sizeof label, "Step %d of %d: %s", progress_.step(), progress_.steps(),
                    progress_.step_name().c_str());
    ImGui::ProgressBar(static_cast<float>(progress_.fraction()), ImVec2(-1, 0), label);
    const std::uint64_t seconds = (SDL_GetTicks() - build_started_at_) / 1000;
    ImGui::TextDisabled("%" PRIu64 " min %02" PRIu64 " s", seconds / 60, seconds % 60);
  }
  if (!build_result_.empty()) {
    coloured(build_failed_ ? kBad : kGood, build_result_);
    if (!work_.empty())
      note("Everything the build printed is in " + utf8(work_ / "build.log") + ".");
  }

  if (!output_.empty()) {
    ImGui::Spacing();
    // What is left of the page, but at least a dozen lines: the page scrolls.
    const float least = ImGui::GetTextLineHeightWithSpacing() * 12;
    const float height = std::max(ImGui::GetContentRegionAvail().y, least);
    ImGui::BeginChild("output", ImVec2(0, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(output_.size()));
    while (clipper.Step())
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        ImGui::TextUnformatted(output_[static_cast<std::size_t>(i)].c_str());
    if (scroll_to_end_) {
      ImGui::SetScrollHereY(1.0f);
      scroll_to_end_ = false;
    }
    ImGui::EndChild();
  }
}

void Launcher::draw_log() {
  heading("Log");
  note("Each run of Bright Dawn writes a log of what happened: when something goes wrong, it says what and "
       "why. Attach it when you report a problem. How much it records is in the settings (Log).");
  const std::filesystem::path file = log::file_path();
  if (file.empty()) {
    coloured(kBad, "This run has no log file.");
    return;
  }
  ImGui::TextWrapped("This run's log: %s", utf8(file).c_str());
  if (ImGui::Button("Open the log folder")) open_folder(file.parent_path());
}

}  // namespace bd::app
