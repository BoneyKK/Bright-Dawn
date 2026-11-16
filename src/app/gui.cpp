#include "app/gui.h"

#include <cmath>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"

namespace bd::app {

Gui::~Gui() { shutdown(); }

void Gui::shutdown() {
  if (!started_) return;
  started_ = false;
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
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

void Gui::set_see_through(bool see_through) {
  if (see_through == see_through_) return;
  see_through_ = see_through;
  if (started_) apply_style();
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
  if (see_through_) {
    // The backdrop shows at the window's edges and faintly through the
    // pages, which stay dark enough to read on.
    c[ImGuiCol_WindowBg].w = 0.0f;
    c[ImGuiCol_ChildBg].w = 0.86f;
    style.WindowBorderSize = 0.0f;
  }
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
  style.ScaleAllSizes(scale);
  style.FontSizeBase = 18.0f;
  style.FontScaleDpi = scale;
  ImGui::GetStyle() = style;
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
