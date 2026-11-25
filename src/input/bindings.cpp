#include "input/bindings.h"

#include <charconv>

namespace bd::input {

const char* name(DsButton button) {
  static constexpr const char* kNames[kDsButtonCount] = {"A",    "B",    "Select", "Start", "Right", "Left",
                                                         "Up",   "Down", "R",      "L",     "X",     "Y"};
  const unsigned index = static_cast<unsigned>(button);
  return index < kDsButtonCount ? kNames[index] : "?";
}

std::optional<DsButton> button_named(std::string_view text) {
  for (unsigned i = 0; i < kDsButtonCount; ++i) {
    const std::string_view candidate = name(static_cast<DsButton>(i));
    if (candidate.size() != text.size()) continue;
    bool same = true;
    for (std::size_t c = 0; c < text.size() && same; ++c) {
      const auto lower = [](char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };
      same = lower(candidate[c]) == lower(text[c]);
    }
    if (same) return static_cast<DsButton>(i);
  }
  return std::nullopt;
}

namespace {

// A frame number of 1 or more: digits only, all of `text`.
std::optional<std::uint64_t> frame_number(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || text.empty() || value == 0) return std::nullopt;
  return value;
}

// "F" or "F-L", with F <= L: the first and last frame.
std::optional<std::pair<std::uint64_t, std::uint64_t>> frame_range(std::string_view frames) {
  const std::size_t dash = frames.find('-');
  const std::optional<std::uint64_t> first = frame_number(frames.substr(0, dash));
  const std::optional<std::uint64_t> last =
      dash == std::string_view::npos ? first : frame_number(frames.substr(dash + 1));
  if (!first || !last || *last < *first) return std::nullopt;
  return std::pair{*first, *last};
}

// A whole number from 0 to `most`: digits only, all of `text`.
std::optional<int> coordinate(std::string_view text, int most) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || text.empty() || text[0] == '-' || value > most)
    return std::nullopt;
  return value;
}

}  // namespace

std::optional<ScriptedPress> parse_scripted_press(std::string_view text) {
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos) return std::nullopt;
  const std::optional<DsButton> button = button_named(text.substr(0, at));
  const auto frames = frame_range(text.substr(at + 1));
  if (!button || !frames) return std::nullopt;
  return ScriptedPress{*button, frames->first, frames->second};
}

std::optional<ScriptedTouch> parse_scripted_touch(std::string_view text) {
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos) return std::nullopt;
  const std::string_view point = text.substr(0, at);
  const std::size_t comma = point.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  const std::optional<int> x = coordinate(point.substr(0, comma), 255);
  const std::optional<int> y = coordinate(point.substr(comma + 1), 191);
  const auto frames = frame_range(text.substr(at + 1));
  if (!x || !y || !frames) return std::nullopt;
  return ScriptedTouch{*x, *y, frames->first, frames->second};
}

std::optional<ScriptedStick> parse_scripted_stick(std::string_view text) {
  const std::size_t at = text.find('@');
  if (at == std::string_view::npos) return std::nullopt;
  const std::string_view point = text.substr(0, at);
  const std::size_t comma = point.find(',');
  if (comma == std::string_view::npos) return std::nullopt;
  // -100 to 100: a sign, then 0 to 100.
  const auto percent = [](std::string_view part) -> std::optional<int> {
    const bool negative = !part.empty() && part[0] == '-';
    const std::optional<int> value = coordinate(negative ? part.substr(1) : part, 100);
    if (!value) return std::nullopt;
    return negative ? -*value : *value;
  };
  const std::optional<int> x = percent(point.substr(0, comma));
  const std::optional<int> y = percent(point.substr(comma + 1));
  const auto frames = frame_range(text.substr(at + 1));
  if (!x || !y || !frames) return std::nullopt;
  return ScriptedStick{*x, *y, frames->first, frames->second};
}

const ScriptedStick* scripted_stick(const std::vector<ScriptedStick>& script, std::uint64_t frame) {
  for (const ScriptedStick& stick : script)
    if (frame >= stick.first && frame <= stick.last) return &stick;
  return nullptr;
}

const ScriptedTouch* scripted_touch(const std::vector<ScriptedTouch>& script, std::uint64_t frame) {
  for (const ScriptedTouch& touch : script)
    if (frame >= touch.first && frame <= touch.last) return &touch;
  return nullptr;
}

std::uint32_t scripted_buttons(const std::vector<ScriptedPress>& script, std::uint64_t frame) {
  std::uint32_t held = 0;
  for (const ScriptedPress& press : script)
    if (frame >= press.first && frame <= press.last) held |= bit(press.button);
  return held;
}

const std::vector<Binding>& default_keyboard_bindings() {
  static const std::vector<Binding> kBindings = {
      {"X", DsButton::A},          {"Z", DsButton::B},       {"S", DsButton::X},
      {"A", DsButton::Y},          {"Q", DsButton::L},       {"W", DsButton::R},
      {"Return", DsButton::Start}, {"Right Shift", DsButton::Select},
      {"Up", DsButton::Up},        {"Down", DsButton::Down}, {"Left", DsButton::Left},
      {"Right", DsButton::Right},
  };
  return kBindings;
}

const std::vector<Binding>& default_controller_bindings() {
  static const std::vector<Binding> kBindings = {
      {"b", DsButton::A},          {"a", DsButton::B},           {"y", DsButton::X},
      {"x", DsButton::Y},          {"leftshoulder", DsButton::L}, {"rightshoulder", DsButton::R},
      {"start", DsButton::Start},  {"back", DsButton::Select},   {"dpup", DsButton::Up},
      {"dpdown", DsButton::Down},  {"dpleft", DsButton::Left},   {"dpright", DsButton::Right},
  };
  return kBindings;
}

std::uint32_t directions_from_stick(int x, int y) {
  std::uint32_t held = 0;
  if (x >= kStickThreshold) held |= bit(DsButton::Right);
  if (x <= -kStickThreshold) held |= bit(DsButton::Left);
  if (y >= kStickThreshold) held |= bit(DsButton::Down);
  if (y <= -kStickThreshold) held |= bit(DsButton::Up);
  return held;
}

}  // namespace bd::input
