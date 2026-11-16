#include "app/menu_widgets.h"

#include <algorithm>

#include "imgui_internal.h"

namespace bd::app::menu {
namespace {

bool styled_button(const char* label, ImVec2 size, const ImVec4& fill, const ImVec4& text, ImFont* font) {
  const float bright = 1.12f;
  const ImVec4 hovered(std::min(fill.x * bright, 1.0f), std::min(fill.y * bright, 1.0f), std::min(fill.z * bright, 1.0f),
                       fill.w);
  ImGui::PushStyleColor(ImGuiCol_Button, fill);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
  ImGui::PushStyleColor(ImGuiCol_Border, skin::kEdge);
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, std::max(2.0f, ImGui::GetFontSize() / 8));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() / 5);
  ImGui::PushFont(font, 0.0f);
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(5);
  return clicked;
}

}  // namespace

bool choice_button(const Look* look, const char* label, bool chosen, ImVec2 size) {
  if (look == nullptr) return ImGui::RadioButton(label, chosen);
  return styled_button(label, size, chosen ? skin::kChosen : skin::kButton, chosen ? skin::kChosenText : skin::kButtonText,
                       look->gui->plain_font());
}

bool action_button(const Look* look, const char* label, ImVec2 size) {
  if (look == nullptr) return ImGui::Button(label, size);
  return styled_button(label, size, skin::kAction, skin::kText, look->gui->outlined_font());
}

void label(const Look* look, const char* text) {
  if (look == nullptr) {
    ImGui::TextUnformatted(text);
    return;
  }
  ImGui::PushFont(look->gui->outlined_font(), 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, skin::kText);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void begin_strip(const Look* look) {
  if (look != nullptr) {
    // The line's widgets go in front; the strip, once the line's size is
    // known, behind them.
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
    const float pad = ImGui::GetFontSize() * 0.6f;
    ImGui::Dummy(ImVec2(0, pad * 0.5f));
    ImGui::Indent(pad);
  }
  ImGui::BeginGroup();
}

bool end_strip(const Look* look) {
  ImGui::EndGroup();
  if (look == nullptr) return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
  // The line's extent, before the space after it is added.
  const ImVec2 group_min = ImGui::GetItemRectMin();
  const ImVec2 group_max = ImGui::GetItemRectMax();
  const float pad = ImGui::GetFontSize() * 0.6f;
  ImGui::Unindent(pad);
  ImGui::Dummy(ImVec2(0, pad * 0.5f));
  ImDrawList* list = ImGui::GetWindowDrawList();
  // Across the page's whole width.
  const ImRect work = ImGui::GetCurrentWindow()->WorkRect;
  const ImVec2 min(work.Min.x, group_min.y - pad * 0.5f), max(std::max(work.Max.x, group_max.x + pad), group_max.y + pad * 0.5f);
  const bool hovered = ImGui::IsMouseHoveringRect(min, max, true) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  list->ChannelsSetCurrent(0);
  look->skin->draw_strip(list, look->renderer, min, max);
  list->ChannelsMerge();
  return hovered;
}

void message_bar(const Look* look, ImDrawList* list, const ImRect& where, const std::string& text) {
  if (look == nullptr || where.GetWidth() <= 0 || where.GetHeight() <= 0) return;
  const float em = ImGui::GetFontSize();
  const float rounding = em * 0.4f, edge = std::max(2.0f, em / 8);
  list->AddRectFilled(where.Min, where.Max, ImGui::GetColorU32(skin::kMessageEdge), rounding);
  const ImRect inner(where.Min.x + edge, where.Min.y + edge, where.Max.x - edge, where.Max.y - edge);
  list->AddRectFilled(inner.Min, inner.Max, ImGui::GetColorU32(skin::kMessage), rounding * 0.8f);
  list->AddRect(ImVec2(inner.Min.x + edge, inner.Min.y + edge), ImVec2(inner.Max.x - edge, inner.Max.y - edge),
                ImGui::GetColorU32(ImVec4(0.75f, 0.86f, 0.95f, 0.55f)), rounding * 0.6f, 0, 1.0f);
  if (text.empty()) return;
  ImFont* font = look->gui->outlined_font();
  const float size = ImGui::GetFontSize();
  const float margin = em * 0.7f;
  const float wrap = inner.GetWidth() - 2 * margin;
  const ImVec2 measured = font->CalcTextSizeA(size, FLT_MAX, wrap, text.c_str());
  const ImVec2 at(inner.Min.x + margin, inner.Min.y + std::max(edge, (inner.GetHeight() - measured.y) / 2));
  list->PushClipRect(inner.Min, inner.Max, true);
  list->AddText(font, size, at, ImGui::GetColorU32(skin::kText), text.c_str(), nullptr, wrap);
  list->PopClipRect();
}

}  // namespace bd::app::menu
