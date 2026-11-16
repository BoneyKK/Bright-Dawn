#include "ui/build_progress.h"

#include <algorithm>

namespace bd::ui {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// A whole number at the start of `s`, taken off it.
std::optional<long long> take_number(std::string_view& s) {
  std::size_t n = 0;
  long long value = 0;
  while (n < s.size() && digit(s[n]) && n < 15) value = value * 10 + (s[n++] - '0');
  if (n == 0) return std::nullopt;
  s.remove_prefix(n);
  return value;
}

}  // namespace

void BuildProgress::take_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.rfind("== ", 0) == 0) {
    std::string_view rest = line.substr(3);
    const auto step = take_number(rest);
    if (step && !rest.empty() && rest.front() == '/') {
      rest.remove_prefix(1);
      const auto steps = take_number(rest);
      if (steps && rest.rfind(". ", 0) == 0 && *step >= 1 && *step <= *steps && *steps <= 100) {
        step_ = static_cast<int>(*step);
        steps_ = static_cast<int>(*steps);
        step_name_ = std::string(rest.substr(2));
        done_ = total_ = -1;
        return;
      }
    }
  }
  if (line.rfind("error: ", 0) == 0) {
    error_ = std::string(line.substr(7));
    return;
  }
  if (line.rfind("The game: ", 0) == 0) {
    game_ = std::string(line.substr(10));
    return;
  }
  if (line.size() >= 2 && line.front() == '[') {
    std::string_view rest = line.substr(1);
    while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
    const auto first = take_number(rest);
    if (!first || rest.empty()) return;
    if (rest.front() == '/') {  // Ninja: [done/total]
      rest.remove_prefix(1);
      const auto second = take_number(rest);
      if (second && !rest.empty() && rest.front() == ']' && *second > 0 && *first <= *second) {
        done_ = *first;
        total_ = *second;
      }
    } else if (rest.rfind("%]", 0) == 0 && *first <= 100) {  // Make: [ nn%]
      done_ = *first;
      total_ = 100;
    }
  }
}

std::optional<double> BuildProgress::within_step() const {
  if (total_ <= 0) return std::nullopt;
  return static_cast<double>(done_) / static_cast<double>(total_);
}

double BuildProgress::fraction() const {
  if (steps_ == 0) return 0.0;
  const double within = within_step().value_or(0.0);
  return std::clamp((step_ - 1 + within) / steps_, 0.0, 1.0);
}

std::optional<std::array<int, 3>> version_in(std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (!digit(text[i]) || (i > 0 && (digit(text[i - 1]) || text[i - 1] == '.'))) continue;
    std::string_view rest = text.substr(i);
    std::array<int, 3> v{0, 0, 0};
    const auto major = take_number(rest);
    if (!major || rest.empty() || rest.front() != '.') continue;
    rest.remove_prefix(1);
    const auto minor = take_number(rest);
    if (!minor) continue;
    v[0] = static_cast<int>(*major);
    v[1] = static_cast<int>(*minor);
    if (!rest.empty() && rest.front() == '.') {
      rest.remove_prefix(1);
      if (const auto patch = take_number(rest)) v[2] = static_cast<int>(*patch);
    }
    return v;
  }
  return std::nullopt;
}

}  // namespace bd::ui
