#include "ui/problem_report.h"

namespace bd::ui {

std::string percent_encode(std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    const bool kept = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '.' || c == '_' || c == '~';
    if (kept) {
      out += ch;
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

std::string problem_report_url(const ProblemReport& report) {
  std::string url = std::string(kNewIssueUrl) + "?template=" + std::string(kBugForm);
  const auto add = [&url](std::string_view field, const std::string& value) {
    if (value.empty()) return;
    url += '&';
    url += field;
    url += '=';
    url += percent_encode(value);
  };
  add("version", report.version);
  add("os", report.os);
  add("renderer", report.renderer);
  add("game-code", report.game_code);
  add("code", report.code);
  return url;
}

}  // namespace bd::ui
