#include "app/gui.h"

#include <cmath>
#include <cstdio>

#include "app/menu_skin.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

namespace bd::app {
namespace {

constexpr float kFontSize = 18.0f;  // Before the display's scale.

}  // namespace

Gui::~Gui() { shutdown(); }

void Gui::shutdown() {
  if (!started_) return;
  started_ = false;
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  plain_ = outlined_ = nullptr;
}

bool Gui::init(SDL_Window* window, SDL_Renderer* renderer, std::string& error) {
  window_ = window;
  renderer_ = renderer;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  // Nothing is written next to the program or in the working folder.
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  // The embedded font that scales well, at the sizes a high-resolution
  // display needs.
  io.Fonts->AddFontDefaultVector();
  if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
    error = "Cannot set up the program's screens for the window";
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
    error = "Cannot set up the program's screens for drawing";
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  started_ = true;
  apply_style();
  return true;
}

void Gui::set_game_look(bool game_look) {
  if (game_look == game_look_) return;
  game_look_ = game_look;
  if (started_) apply_style();
}

void Gui::set_game_font(std::shared_ptr<const assets::Font> font) {
  if (!started_) return;
  ImFontAtlas* atlas = ImGui::GetIO().Fonts;
  if (plain_ != nullptr) {
    ImGui::GetIO().FontDefault = nullptr;
    atlas->RemoveFont(outlined_);
    atlas->RemoveFont(plain_);
    plain_ = outlined_ = nullptr;
  }
  plain_source_ = GameFontSource{font, false};
  outlined_source_ = GameFontSource{font, true};
  if (font != nullptr) {
    const auto add = [&](GameFontSource& source, const char* name) {
      ImFontConfig config;
      config.FontLoader = game_font_loader();
      config.FontData = &source;
      config.FontDataSize = 1;
      config.FontDataOwnedByAtlas = false;
      std::snprintf(config.Name, sizeof config.Name, "%s", name);
      ImFont* added = atlas->AddFont(&config);
      // Characters the game's font lacks: from the program's own.
      ImFontConfig fallback;
      fallback.MergeMode = true;
      fallback.DstFont = added;
      atlas->AddFontDefaultVector(&fallback);
      return added;
    };
    plain_ = add(plain_source_, "game font");
    outlined_ = add(outlined_source_, "game font, outlined");
  }
  apply_style();
}

ImFont* Gui::outlined_font() const { return outlined_ != nullptr ? outlined_ : ImGui::GetIO().Fonts->Fonts[0]; }

ImFont* Gui::plain_font() const { return plain_ != nullptr ? plain_ : ImGui::GetIO().Fonts->Fonts[0]; }

float Gui::sharp_size(float wanted) const {
  if (plain_ == nullptr || plain_source_.font == nullptr) return wanted;
  const assets::Font& font = *plain_source_.font;
  return static_cast<float>(font.line_height() * game_font_scale(font, wanted));
}

void Gui::apply_style() {
  float scale = SDL_GetWindowDisplayScale(window_);
  if (!(scale > 0.25f && scale < 8.0f)) scale = 1.0f;
  scale_ = scale;
  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);
  // Warm, dark pages with golden headings: a start on the settings book's
  // look, made of nothing but colours.
  ImVec4* c = style.Colors;
  const ImVec4 page(0.11f, 0.10f, 0.09f, 1.00f), panel(0.16f, 0.14f, 0.12f, 1.00f);
  const ImVec4 gold(0.86f, 0.66f, 0.28f, 1.00f), gold_dim(0.55f, 0.42f, 0.18f, 1.00f);
  c[ImGuiCol_WindowBg] = page;
  c[ImGuiCol_ChildBg] = panel;
  c[ImGuiCol_PopupBg] = panel;
  c[ImGuiCol_Border] = ImVec4(0.35f, 0.29f, 0.20f, 1.00f);
  c[ImGuiCol_Text] = ImVec4(0.93f, 0.89f, 0.80f, 1.00f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.57f, 0.49f, 1.00f);
  c[ImGuiCol_FrameBg] = ImVec4(0.22f, 0.19f, 0.15f, 1.00f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.25f, 0.18f, 1.00f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.36f, 0.30f, 0.20f, 1.00f);
  c[ImGuiCol_Button] = ImVec4(0.33f, 0.26f, 0.14f, 1.00f);
  c[ImGuiCol_ButtonHovered] = gold_dim;
  c[ImGuiCol_ButtonActive] = gold;
  c[ImGuiCol_Header] = ImVec4(0.33f, 0.26f, 0.14f, 1.00f);
  c[ImGuiCol_HeaderHovered] = gold_dim;
  c[ImGuiCol_HeaderActive] = gold;
  c[ImGuiCol_CheckMark] = gold;
  c[ImGuiCol_SliderGrab] = gold_dim;
  c[ImGuiCol_SliderGrabActive] = gold;
  c[ImGuiCol_PlotHistogram] = gold;
  c[ImGuiCol_Separator] = ImVec4(0.35f, 0.29f, 0.20f, 1.00f);
  c[ImGuiCol_NavCursor] = gold;
  c[ImGuiCol_TitleBg] = ImVec4(0.20f, 0.16f, 0.10f, 1.00f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.33f, 0.26f, 0.14f, 1.00f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.20f, 0.16f, 0.10f, 1.00f);
  c[ImGuiCol_Tab] = ImVec4(0.22f, 0.19f, 0.15f, 1.00f);
  c[ImGuiCol_TabHovered] = gold_dim;
  c[ImGuiCol_TabSelected] = ImVec4(0.33f, 0.26f, 0.14f, 1.00f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.33f, 0.26f, 0.14f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = gold_dim;
  c[ImGuiCol_ScrollbarGrabActive] = gold;
  c[ImGuiCol_ResizeGrip] = ImVec4(0.33f, 0.26f, 0.14f, 0.50f);
  c[ImGuiCol_ResizeGripHovered] = gold_dim;
  c[ImGuiCol_ResizeGripActive] = gold;
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.42f, 0.18f, 0.50f);
  style.WindowRounding = 0;
  style.ChildRounding = 4;
  style.FrameRounding = 4;
  style.GrabRounding = 4;
  style.FramePadding = ImVec2(8, 5);
  style.ItemSpacing = ImVec2(10, 8);
  style.WindowPadding = ImVec2(16, 16);
  if (game_look_) apply_game_look(style);
  style.ScaleAllSizes(scale);
  if (plain_ != nullptr) {
    // Whole screen pixels for each of the font's, near the size the
    // program's own font would have.
    const assets::Font& font = *plain_source_.font;
    const int k = game_font_scale(font, kFontSize * scale);
    style.FontSizeBase = static_cast<float>(font.line_height() * k);
    style.FontScaleDpi = 1.0f;
  } else {
    style.FontSizeBase = kFontSize;
    style.FontScaleDpi = scale;
  }
  ImGui::GetStyle() = style;
  ImGui::GetIO().FontDefault = plain_;
}

// The menus' colours (app/menu_skin.h) on parchment and wood: the pages
// draw nothing of their own, the pictures behind them show.
void Gui::apply_game_look(ImGuiStyle& style) {
  ImVec4* c = style.Colors;
  const ImVec4 clear(0, 0, 0, 0);
  const ImVec4 paper_dark(0.80f, 0.69f, 0.50f, 1.0f), paper_darker(0.72f, 0.60f, 0.42f, 1.0f);
  const ImVec4 brown(0.56f, 0.38f, 0.16f, 1.0f), brown_light(0.70f, 0.50f, 0.24f, 1.0f);
  c[ImGuiCol_WindowBg] = clear;
  c[ImGuiCol_ChildBg] = clear;
  c[ImGuiCol_Border] = brown;
  c[ImGuiCol_PopupBg] = ImVec4(0.93f, 0.86f, 0.70f, 1.0f);
  c[ImGuiCol_Text] = skin::kInk;
  c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.33f, 0.20f, 1.0f);
  c[ImGuiCol_FrameBg] = paper_dark;
  c[ImGuiCol_FrameBgHovered] = paper_darker;
  c[ImGuiCol_FrameBgActive] = paper_darker;
  c[ImGuiCol_Button] = skin::kAction;
  c[ImGuiCol_ButtonHovered] = ImVec4(0.36f, 0.66f, 0.95f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.42f, 0.72f, 1.0f);
  c[ImGuiCol_Header] = paper_dark;
  c[ImGuiCol_HeaderHovered] = paper_darker;
  c[ImGuiCol_HeaderActive] = brown_light;
  c[ImGuiCol_CheckMark] = skin::kChosenText;
  c[ImGuiCol_SliderGrab] = skin::kAction;
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.16f, 0.42f, 0.72f, 1.0f);
  c[ImGuiCol_PlotHistogram] = skin::kAction;
  c[ImGuiCol_Separator] = brown;
  c[ImGuiCol_NavCursor] = skin::kChosen;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.10f);
  c[ImGuiCol_ScrollbarGrab] = brown;
  c[ImGuiCol_ScrollbarGrabHovered] = brown_light;
  c[ImGuiCol_ScrollbarGrabActive] = skin::kChosenText;
  c[ImGuiCol_Tab] = paper_dark;
  c[ImGuiCol_TabHovered] = paper_darker;
  c[ImGuiCol_TabSelected] = skin::kChosen;
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.22f, 0.53f, 0.85f, 0.35f);
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 0.0f;
  style.FrameBorderSize = 1.0f;
  style.WindowPadding = ImVec2(12, 12);
}

void Gui::process_event(const SDL_Event& event) {
  ImGui_ImplSDL3_ProcessEvent(&event);
  if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) apply_style();
}

void Gui::begin_frame() {
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

void Gui::end_frame() {
  ImGui::Render();
  // With the renderer's own scale left at 1, the backend scales from window
  // coordinates to the window's pixels itself.
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
}

}  // namespace bd::app
