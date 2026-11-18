#include "app/launcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/version.h"
#include "game_library_host.h"
#include "imgui.h"
#include "platform/paths.h"
#include "rom/game_id.h"
#include "rom/rom.h"
#include "ui/game_settings.h"

namespace bd::app {
namespace {

constexpr std::size_t kMostOutputLines = 5000;

// Good and bad news: on the program's dark pages, and on parchment.
const ImVec4 kGood(0.55f, 0.82f, 0.45f, 1.0f);
const ImVec4 kBad(0.93f, 0.45f, 0.38f, 1.0f);
const ImVec4 kGoodOnPaper(0.12f, 0.40f, 0.08f, 1.0f);
const ImVec4 kBadOnPaper(0.66f, 0.12f, 0.06f, 1.0f);

constexpr ImGuiWindowFlags kWholeWindow = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;


}  // namespace

Launcher::Launcher(App& app) : app_(app) {
  look_ = menu::Look{&skin_, app_.renderer, &app_.gui};
  if (!app_.user_data.empty()) library_ = game_library_path(app_.user_data);
  if (Result<std::filesystem::path> self = platform::executable_path(); self) program_ = self.value();
  const std::string remembered = app_.settings.value(ui::key::kRom);
  if (!remembered.empty()) choose_rom(from_utf8(remembered));
  builder_ = find_builder();
  check_built_game();
  setup_ = needs_setup();
  if (setup_) {
    log::info("launcher", built_state_ == BuiltState::None ? "the setup: the game is not built yet"
                                                           : "the setup: the game built before cannot be used");
    check_tools();
  }
}

bool Launcher::needs_setup() const { return !app_.translated_build && built_state_ != BuiltState::Ready; }

Launcher::~Launcher() {
  fetch_.reset();
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
    app_.report.game_code = title->game_code;
    rom_check_.text = std::string("Golden Sun: Dark Dawn (") + rom::to_string(title->region) + ", " +
                      std::string(title->game_code) + ")";
  }
  if (rom_check_.ok) read_menu_art(rom.value());
  log::writef(rom_check_.ok ? log::Level::Info : log::Level::Warning, "launcher", "ROM %s: %s", utf8(path).c_str(),
              rom_check_.text.c_str());
  if (rom_check_.ok && app_.settings.value(ui::key::kRom) != utf8(path)) app_.set_setting(ui::key::kRom, utf8(path));
}

void Launcher::read_menu_art(const rom::Rom& rom) {
  skin_.read(rom);
  skin_changed_ = true;
}

void Launcher::check_tools() {
  python_ = find_python();
  builder_ = find_builder();
  tools_checked_ = true;
  log::info("builder", "builder files: " + (builder_ ? utf8(*builder_) : std::string("not found")));
  log::info("builder", python_.found ? "Python " + python_.version + " (" + python_.command[0] + ")" : python_.problem);
}

void Launcher::check_built_game() {
  built_state_ = BuiltState::None;
  built_detail_.clear();
  if (app_.translated_build || library_.empty()) return;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(library_, ec)) return;
  // Loaded to be checked, and let go again: it is loaded for good when the
  // game is played, and must not be held while the builder replaces it.
  runtime::GameLibrary library;
  if (library.open(library_, BD_VERSION)) {
    built_state_ = BuiltState::Ready;
    built_detail_ = std::string("built for ") + library.built_for();
  } else {
    built_detail_ = library.error();
    built_state_ = built_detail_.find("was built for") != std::string::npos ? BuiltState::OtherVersion
                                                                          : BuiltState::Broken;
  }
  log::info("launcher", "game library " + utf8(library_) + ": " +
                            (built_state_ == BuiltState::Ready ? "ready, " : "not usable: ") + built_detail_);
}

void Launcher::begin_build_output() {
  output_.clear();
  progress_ = ui::BuildProgress();
  build_result_.clear();
  build_failed_ = false;
  stopping_ = false;
  build_started_at_ = SDL_GetTicks();
}

void Launcher::add_output(std::string_view text) {
  output_.emplace_back(text);
  if (output_.size() > kMostOutputLines) output_.pop_front();
  scroll_to_end_ = true;
}

void Launcher::start_build() {
  if (building()) return;
  begin_build_output();
  if (python_.found) {
    start_builder(python_.command);
    return;
  }
  // No Python here: one is fetched for this build, and removed after it.
  const std::optional<Download> download = python_download();
  if (!download) {
    finish_build(false, python_.problem + ": install Python 3.10 or later");
    return;
  }
  fetch_ = std::make_unique<Fetch>();
  std::string error;
  add_output("Getting " + download->what + " for the build (" + std::to_string(download->size / (1024 * 1024)) +
             " MB), from " + download->url);
  if (!fetch_->start(*download, error)) {
    fetch_.reset();
    finish_build(false, download->what + " could not be fetched: " + error);
  }
}

void Launcher::start_builder(const std::vector<std::string>& python) {
  std::vector<std::string> args = python;
  args.push_back(utf8(*builder_ / "tools" / "gamebuilder" / "build_game.py"));
  args.push_back(utf8(rom_));
  args.push_back("--program");
  args.push_back(utf8(program_));
  args.push_back("--out");
  args.push_back(utf8(library_));
  args.push_back("--stop-on-stdin-eof");
  const long long jobs = app_.settings.number(ui::key::kBuildJobs);
  if (jobs > 0) {
    args.push_back("--jobs");
    args.push_back(std::to_string(jobs));
  }
  if (const std::string compiler = app_.settings.value(ui::key::kBuildCompiler); !compiler.empty()) {
    args.push_back("--compiler");
    args.push_back(compiler);
  }
  std::string line;
  for (const std::string& a : args) line += (line.empty() ? "" : " ") + a;
  log::info("builder", "starting: " + line);
  build_ = std::make_unique<ChildProcess>();
  std::string error;
  if (!build_->start(args, true, error)) {
    build_.reset();
    finish_build(false, "The builder could not be started: " + error);
  }
}

void Launcher::stop_build() {
  if (!building() || stopping_) return;
  log::info("builder", "stopping the build, as asked");
  stopping_ = true;
  stop_asked_at_ = SDL_GetTicks();
  if (fetch_ != nullptr) fetch_->stop();
  if (build_ != nullptr) build_->close_input();
}

void Launcher::finish_build(bool ok, const std::string& result) {
  const std::uint64_t seconds = (SDL_GetTicks() - build_started_at_) / 1000;
  char took[64];
  std::snprintf(took, sizeof took, " (%" PRIu64 " min %" PRIu64 " s)", seconds / 60, seconds % 60);
  build_failed_ = !ok;
  build_result_ = result.empty() ? std::string("The game was built") + took + "." : result;
  (build_failed_ ? log::warning : log::info)("builder", build_result_);
  build_.reset();
  // Python fetched for the build goes with it.
  fetch_.reset();
  stopping_ = false;
  check_built_game();
}

void Launcher::poll_build() {
  if (fetch_ != nullptr && build_ == nullptr) {
    fetch_->poll([&](std::string_view text) { add_output(text); });
    if (fetch_->state() == Fetch::State::Done) {
      start_builder({utf8(fetch_->program())});
    } else if (fetch_->state() == Fetch::State::Failed) {
      finish_build(false, stopping_ ? "The build was stopped." : "Python could not be fetched: " + fetch_->error());
    }
    return;
  }
  if (build_ == nullptr) return;
  build_->poll([&](std::string_view text) {
    progress_.take_line(text);
    if (text.rfind("== ", 0) == 0 || text.rfind("error: ", 0) == 0 || text.rfind("The game: ", 0) == 0)
      log::info("builder", std::string(text));
    add_output(text);
  });
  if (stopping_ && build_->running() && SDL_GetTicks() - stop_asked_at_ > 20000) {
    log::warning("builder", "the builder did not stop when asked: it is ended");
    build_->kill();
  }
  if (!build_->ended()) return;
  const bool ok = (!ChildProcess::kExitCodeKnown || build_->exit_code() == 0) && !progress_.game().empty();
  if (stopping_) {
    finish_build(false, "The build was stopped.");
  } else if (ok) {
    finish_build(true, "");
  } else {
    const std::uint64_t seconds = (SDL_GetTicks() - build_started_at_) / 1000;
    char took[64];
    std::snprintf(took, sizeof took, " (%" PRIu64 " min %" PRIu64 " s)", seconds / 60, seconds % 60);
    finish_build(false, "The build failed" + std::string(took) + ": " +
                            (progress_.error().empty() ? "exit status " + std::to_string(build_->exit_code())
                                                       : progress_.error()) +
                            ".");
  }
}

void Launcher::play(LauncherChoice& choice) {
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
    if (rom_asked_) {
      choose_rom(*rom_asked_);
      rom_asked_.reset();
    }
    poll_build();
    if (done) break;
    // The game's menus, read from a ROM just chosen: their font from now on.
    if (skin_changed_) {
      skin_changed_ = false;
      app_.gui.set_game_font(skin_.ready() ? skin_.font() : nullptr);
    }
    app_.gui.set_game_look(look() != nullptr);
    book_.set_look(look());
    SDL_SetRenderDrawColor(app_.renderer, 0, 0, 0, 255);
    SDL_RenderClear(app_.renderer);
    if (look() != nullptr) skin_.draw_background(app_.renderer);
    app_.gui.begin_frame();
    draw(choice, done);
    app_.gui.end_frame();
    SDL_RenderPresent(app_.renderer);
    if (!vsync) SDL_Delay(15);
  }
  // The game's screens are in the program's own look.
  book_.set_look(nullptr);
  app_.gui.set_game_look(false);
  app_.gui.set_game_font(nullptr);
  skin_.release();
  return choice;
}

void Launcher::draw(LauncherChoice& choice, bool& done) {
  hint_.clear();
  if (look() != nullptr) {
    draw_dressed(choice, done);
    return;
  }
  if (setup_) {
    draw_setup(choice, done);
    return;
  }
  const ImGuiViewport* view = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(view->Pos);
  ImGui::SetNextWindowSize(view->Size);
  ImGui::Begin("Bright Dawn", nullptr, kWholeWindow);
  // The title on a panel of its own.
  ImGui::BeginChild("title", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::TextUnformatted("Bright Dawn");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s%s", BD_VERSION, app_.translated_build ? ", the built game" : "");
  ImGui::EndChild();

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

void Launcher::draw_setup(LauncherChoice& choice, bool& done) {
  const ImGuiViewport* view = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  const float margin = std::min(view->Size.x, view->Size.y) * 0.05f;
  const float width = std::min(view->Size.x - 2 * margin, em * 50);
  ImGui::SetNextWindowPos(ImVec2(view->Pos.x + (view->Size.x - width) / 2, view->Pos.y + margin));
  ImGui::SetNextWindowSize(ImVec2(width, view->Size.y - 2 * margin));
  ImGui::Begin("Setup", nullptr, kWholeWindow);
  ImGui::BeginChild("setup page", ImVec2(0, 0), ImGuiChildFlags_Borders);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::TextUnformatted("Bright Dawn");
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s, setting up", BD_VERSION);
  ImGui::SameLine(ImGui::GetContentRegionMax().x - em * 5);
  if (ImGui::Button("Quit", ImVec2(em * 5, 0))) {
    log::info("launcher", "quit from the setup");
    choice.kind = LauncherChoice::Quit;
    done = true;
  }
  ImGui::Separator();
  draw_setup_page();
  ImGui::EndChild();
  ImGui::End();
}

void Launcher::draw_setup_page() {
  if (built_state_ == BuiltState::Ready) {
    coloured(true, "The game is built: go on to the launcher, below the build's result.");
  } else if (built_state_ == BuiltState::None) {
    note("Before the launcher opens, the game is built once from your ROM: its code translated to C++ and "
         "compiled for this computer, so that it runs natively. Choose your ROM, then build. What the build "
         "needs and this computer does not have is fetched for it and removed afterwards.");
  } else {
    note("The game built before cannot be used (" + built_detail_ +
         "): Bright Dawn has changed since, and the game has to be built again for this version.");
  }
  ImGui::Spacing();
  draw_rom_chooser();
  ImGui::Spacing();
  ImGui::Separator();
  draw_build();
}

void Launcher::draw_dressed(LauncherChoice& choice, bool& done) {
  SDL_Renderer* renderer = app_.renderer;
  const ImGuiViewport* view = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(view->Pos);
  ImGui::SetNextWindowSize(view->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("Bright Dawn", nullptr,
               kWholeWindow | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImDrawList* list = ImGui::GetWindowDrawList();
  const float em = ImGui::GetFontSize();
  const float scale = skin_.picture_scale(renderer);

  // The title on its ribbon, in the sky between the upper gears.
  const char* title = "Bright Dawn";
  const float ribbon_height = 20.0f * scale;
  ImFont* outlined = app_.gui.outlined_font();
  const float title_size = app_.gui.sharp_size(std::max(em, ribbon_height * 0.5f));
  const ImVec2 title_extent = outlined->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title);
  const ImRect band = skin_.draw_ribbon(list, renderer, skin_.at(renderer, 128.0f, skin::kTitleY), ribbon_height,
                                        title_extent.x);
  list->AddText(outlined, title_size,
                ImVec2(std::floor(band.GetCenter().x - title_extent.x / 2),
                       std::floor(band.GetCenter().y - title_extent.y / 2)),
                ImGui::GetColorU32(skin::kText), title);

  // The pages, on the board.
  const ImRect board = skin_.board(renderer);
  const float inset = std::max(em * 0.5f, 5.0f * scale);
  ImGui::SetCursorScreenPos(ImVec2(board.Min.x + inset, board.Min.y + inset));
  ImGui::BeginChild("board", ImVec2(std::max(1.0f, board.GetWidth() - 2 * inset), std::max(1.0f, board.GetHeight() - 2 * inset)),
                    ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
  if (setup_) draw_setup_dressed(choice, done);
  else draw_pages_dressed(choice, done);
  ImGui::EndChild();

  // What is under the pointer, or what to do, in the message bar.
  menu::message_bar(look(), list,
                    skin_.area(renderer, skin::kMessageLeft, skin::kMessageTop, skin::kMessageRight,
                               skin::kMessageBottom),
                    hint_.empty() ? standing_hint() : hint_);
  ImGui::End();
}

void Launcher::draw_pages_dressed(LauncherChoice& choice, bool& done) {
  const float em = ImGui::GetFontSize();
  ImGui::BeginChild("pages", ImVec2(em * 8, 0));
  const struct {
    Page page;
    const char* name;
    const char* hint;
  } pages[] = {{Page::Play, "Play", "Choose your ROM, and play."},
               {Page::Build, "Build", "Build the game from your ROM, so that its code runs natively."},
               {Page::Settings, "Settings", "How the game is shown and played, and how it is built."},
               {Page::Log, "Log", "Where this run's log is, for when something goes wrong."}};
  for (const auto& p : pages) {
    if (p.page == Page::Build && app_.translated_build) continue;
    if (menu::choice_button(look(), p.name, page_ == p.page, ImVec2(-1.0f, em * 1.7f))) {
      page_ = p.page;
      if (page_ == Page::Build && !tools_checked_) check_tools();
    }
    explain(p.hint);
  }
  ImGui::EndChild();
  ImGui::SameLine(0.0f, em * 0.6f);
  if (page_ == Page::Settings) {
    // The settings book lays its strips of parchment on the wood itself.
    ImGui::BeginChild("page", ImVec2(0, 0));
    app_.settings_changed(book_.draw(app_.settings));
    ImGui::EndChild();
    if (!book_.hovered_help().empty()) hint_ = book_.hovered_help();
    return;
  }
  begin_paper("page");
  switch (page_) {
    case Page::Play: draw_play(choice, done); break;
    case Page::Build: draw_build(); break;
    case Page::Settings: break;
    case Page::Log: draw_log(); break;
  }
  end_paper();
}

void Launcher::draw_setup_dressed(LauncherChoice& choice, bool& done) {
  begin_paper("setup page");
  const float em = ImGui::GetFontSize();
  heading("Setting up");
  const float quit_width = em * 4.5f;
  ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionMax().x - quit_width));
  if (button("Quit", ImVec2(quit_width, 0))) {
    log::info("launcher", "quit from the setup");
    choice.kind = LauncherChoice::Quit;
    done = true;
  }
  explain("Close Bright Dawn. The setup goes on where it was the next time.");
  ImGui::Separator();
  draw_setup_page();
  end_paper();
}

void Launcher::begin_paper(const char* id) {
  const float em = ImGui::GetFontSize();
  const ImVec2 min = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  const ImVec2 max(min.x + size.x, min.y + size.y);
  ImDrawList* list = ImGui::GetWindowDrawList();
  skin_.draw_paper(list, app_.renderer, min, max);
  list->AddRect(min, max, ImGui::GetColorU32(skin::kMessageEdge), 0.0f, 0, std::max(2.0f, em / 10));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em, em * 0.8f));
  ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
}

void Launcher::end_paper() { ImGui::EndChild(); }

void Launcher::heading(const char* text) const {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
  ImGui::PushFont(nullptr, app_.gui.sharp_size(ImGui::GetStyle().FontSizeBase * 1.4f));
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::PopStyleColor();
}

void Launcher::note(const std::string& text) const {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

ImVec4 Launcher::colour(bool good) const {
  if (look() != nullptr) return good ? kGoodOnPaper : kBadOnPaper;
  return good ? kGood : kBad;
}

void Launcher::coloured(bool good, const std::string& text) const {
  ImGui::PushStyleColor(ImGuiCol_Text, colour(good));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
}

bool Launcher::button(const char* label, ImVec2 size) const { return menu::action_button(look(), label, size); }

bool Launcher::small_button(const char* label) const {
  return look() != nullptr ? menu::action_button(look(), label) : ImGui::SmallButton(label);
}

void Launcher::explain(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) hint_ = text;
}

std::string Launcher::standing_hint() const {
  const std::string name = std::string("Bright Dawn ") + BD_VERSION;
  if (building()) return "The game is being built from your ROM. This takes some minutes.";
  if (setup_) {
    if (built_state_ == BuiltState::Ready) return "The game is built. Go on to the launcher.";
    return name + ": the game is built once from your ROM, then the launcher opens.";
  }
  switch (page_) {
    case Page::Play: return rom_check_.ok ? name + ": ready to play." : name + ": choose your ROM.";
    case Page::Build: return "The game is built from your ROM, and its code then runs natively.";
    case Page::Settings: return "Point at a setting to see what it does.";
    case Page::Log: return "Attach the log when you report a problem.";
  }
  return name;
}

void Launcher::draw_rom_chooser() {
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
  const bool choose = button("Choose your ROM...");
  explain("Choose your own copy of Golden Sun: Dark Dawn: a .nds file.");
  if (choose) {
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
    rom_asked_ = from_utf8(rom_typed_);
  } else {
    rom_typed_ = buffer;
  }
  ImGui::SameLine();
  if (button("Use this")) rom_asked_ = from_utf8(rom_typed_);
  explain("Use the ROM at the path typed beside this.");
  if (!rom_.empty()) coloured(rom_check_.ok, rom_check_.text);
}

void Launcher::draw_play(LauncherChoice& choice, bool& done) {
  draw_rom_chooser();
  ImGui::Spacing();
  heading("Play");
  if (app_.translated_build) {
    note("This is a build with the game's code linked in: it runs natively.");
  } else if (built_state_ == BuiltState::Ready && app_.settings.toggle(ui::key::kPlayBuilt)) {
    note("The built game is played: its code runs natively.");
  } else if (built_state_ == BuiltState::Ready) {
    note("The game is interpreted: playing the built game is turned off (Settings, Game).");
  } else {
    note("The game is interpreted, which is slow: about a third of the console's speed. Build the game "
         "(Build the game, on the left) to run its code natively.");
    if (built_state_ == BuiltState::OtherVersion)
      note("The game built before cannot be used: " + built_detail_ + ". Build it again.");
    else if (built_state_ == BuiltState::Broken)
      note("The game built before cannot be used: " + built_detail_ + ". Build it again.");
  }
  ImGui::BeginDisabled(!rom_check_.ok);
  if (button("Play", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f))) {
    play(choice);
    done = true;
  }
  ImGui::EndDisabled();
  explain(rom_check_.ok ? "Play Golden Sun: Dark Dawn from your ROM." : "Choose your ROM first.");
  ImGui::Spacing();
  note("Keyboard: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Right Shift = Select. "
       "Controllers: by position, as on the DS. The left mouse button on the touch screen is the stylus. "
       "Escape (or a controller's guide button) pauses and opens the menu. F11: full screen.");
}

void Launcher::draw_build() {
  heading("Build the game");
  note("The builder translates the game's code, from your ROM, to C++ and compiles it for this computer, "
       "into a library the program loads: the game then runs natively instead of being interpreted. The "
       "translated code is made from your ROM and stays on this computer. A build takes some minutes on a "
       "fast computer, longer on a slower one.");
  ImGui::Spacing();
  ImGui::TextUnformatted("What it needs:");
  const auto requirement = [this](bool ok, const std::string& what, const std::string& detail) {
    ImGui::Bullet();
    ImGui::PushStyleColor(ImGuiCol_Text, colour(ok));
    ImGui::TextUnformatted(what.c_str());
    ImGui::PopStyleColor();
    if (!detail.empty()) {
      ImGui::Indent();
      note(detail);
      ImGui::Unindent();
    }
  };
  requirement(rom_check_.ok, rom_check_.ok ? "Your ROM" : setup_ ? "Your ROM: choose it above" : "Your ROM: choose it on the Play page", "");
  requirement(builder_.has_value(), builder_ ? "The builder's files" : "The builder's files were not found",
              builder_ ? utf8(*builder_)
                       : "They come with Bright Dawn, in the folder \"builder\" next to the program.");
  const std::optional<Download> python = python_download();
  if (python_.found) {
    requirement(true, "Python " + python_.version, "");
  } else if (python) {
    requirement(true, "Python: " + python->what + " is fetched for the build, and removed after it",
                python_.problem + " on this computer.");
  } else {
    requirement(false, python_.problem,
                "Install Python 3.10 or later (from your system's packages, or python.org), then check again.");
  }
  ImGui::Bullet();
  if (const std::string compiler = app_.settings.value(ui::key::kBuildCompiler); compiler.empty())
    ImGui::TextWrapped("The compiler: Zig (Clang), about 95 MB, is fetched by the builder for the build and removed "
                       "after it. It needs an internet connection, and about 2 GB of free space while it builds.");
  else
    ImGui::TextWrapped("The compiler: %s, as chosen in the settings (Builder). The build needs about 2 GB of free "
                       "space while it runs.", compiler.c_str());
  const bool running = building();
  ImGui::BeginDisabled(running);
  if (button("Check again")) check_tools();
  ImGui::EndDisabled();
  explain("Look again for what the build needs.");

  ImGui::Spacing();
  ImGui::TextUnformatted("The built game:");
  ImGui::SameLine();
  ImGui::TextWrapped("%s", library_.empty() ? "(no user data folder)" : utf8(library_).c_str());
  if (!library_.empty()) {
    if (small_button("Open the folder")) open_folder(library_.parent_path());
    explain("Open the folder the built game is in.");
  }
  if (built_state_ == BuiltState::Ready) coloured(true, "Built, and ready: " + built_detail_);
  else if (built_state_ != BuiltState::None) coloured(false, "Built, but it cannot be used: " + built_detail_);

  ImGui::Spacing();
  const bool can_build =
      rom_check_.ok && builder_ && (python_.found || python) && !library_.empty() && !program_.empty();
  if (!running) {
    ImGui::BeginDisabled(!can_build);
    if (button(built_state_ != BuiltState::None ? "Build it again" : "Build the game",
               ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f)))
      start_build();
    ImGui::EndDisabled();
    explain(can_build ? "Translate the game's code to C++ and compile it for this computer: some minutes."
                      : "What the build needs is not all here yet: see above.");
  } else {
    ImGui::BeginDisabled(stopping_);
    if (button(stopping_ ? "Stopping..." : "Stop", ImVec2(ImGui::GetFontSize() * 10, ImGui::GetFontSize() * 2.2f)))
      stop_build();
    ImGui::EndDisabled();
    explain("Stop the build. It starts from the beginning the next time.");
    char label[160];
    if (fetch_ != nullptr && build_ == nullptr)
      std::snprintf(label, sizeof label, "Getting Python for the build");
    else if (progress_.steps() == 0)
      std::snprintf(label, sizeof label, "Starting");
    else
      std::snprintf(label, sizeof label, "Step %d of %d: %s", progress_.step(), progress_.steps(),
                    progress_.step_name().c_str());
    ImGui::ProgressBar(static_cast<float>(progress_.fraction()), ImVec2(-1, 0), label);
    const std::uint64_t seconds = (SDL_GetTicks() - build_started_at_) / 1000;
    ImGui::TextDisabled("%" PRIu64 " min %02" PRIu64 " s", seconds / 60, seconds % 60);
  }
  if (!build_result_.empty()) {
    coloured(!build_failed_, build_result_);
    if (!library_.empty())
      note("Everything the build printed is in " + utf8(library_.parent_path() / "build.log") + ".");
  }
  if (setup_ && !running && built_state_ == BuiltState::Ready) {
    const bool go_on = button("Go on to the launcher", ImVec2(ImGui::GetFontSize() * 14, ImGui::GetFontSize() * 2.2f));
    explain("The setup is done: open the launcher.");
    if (go_on) {
      log::info("launcher", "the setup is done");
      setup_ = false;
      page_ = Page::Play;
    }
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
    coloured(false, "This run has no log file.");
    return;
  }
  ImGui::TextWrapped("This run's log: %s", utf8(file).c_str());
  if (button("Open the log folder")) open_folder(file.parent_path());
  explain("Open the folder the logs are in. The newest log is this run's.");
  if (button("Report a problem")) {
    const bool built = app_.translated_build ||
                       (built_state_ == BuiltState::Ready && app_.settings.toggle(ui::key::kPlayBuilt));
    app_.report.code = built ? "The built game" : "The interpreter";
    report_problem(app_);
  }
  explain("Open the form for a problem report on GitHub, filled in with this computer and version, and the "
          "log folder. Attach the log; never the ROM, saves or other game files.");
}

}  // namespace bd::app
