#include "ui/update_check.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace bd::ui {
namespace {

// A whole number at `at` (at most 9 digits), moving `at` past it.
std::optional<int> number(std::string_view text, std::size_t& at) {
  const std::size_t start = at;
  int value = 0;
  while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0 && at - start < 9)
    value = value * 10 + (text[at++] - '0');
  if (at == start) return std::nullopt;
  return value;
}

// Reads JSON as far as the API's list of releases needs: the strings and
// true/false of each object's own members, at the top level of the array.
class Reader {
 public:
  explicit Reader(std::string_view text) : text_(text) {}

  std::optional<std::vector<Release>> releases() {
    std::vector<Release> list;
    space();
    if (!take('[')) return std::nullopt;
    space();
    if (take(']')) return finished(list);
    for (;;) {
      Release release;
      if (!object(release)) return std::nullopt;
      list.push_back(std::move(release));
      space();
      if (take(']')) return finished(list);
      if (!take(',')) return std::nullopt;
      space();
    }
  }

 private:
  std::optional<std::vector<Release>> finished(std::vector<Release>& list) {
    space();
    if (at_ != text_.size()) return std::nullopt;
    return std::move(list);
  }

  bool object(Release& release) {
    if (!take('{')) return false;
    space();
    if (take('}')) return true;
    for (;;) {
      std::string key;
      if (!string(key)) return false;
      space();
      if (!take(':')) return false;
      space();
      if (key == "tag_name" || key == "html_url") {
        std::string value;
        if (peek() == '"') {
          if (!string(value)) return false;
        } else if (!skip_value(0)) {
          return false;
        }
        (key == "tag_name" ? release.tag : release.page) = value;
      } else if (key == "draft" && text_.substr(at_, 4) == "true") {
        release.draft = true;
        at_ += 4;
      } else if (!skip_value(0)) {
        return false;
      }
      space();
      if (take('}')) return true;
      if (!take(',')) return false;
      space();
    }
  }

  // A value of any kind, unread.
  bool skip_value(int depth) {
    if (depth > 64) return false;
    space();
    const char c = peek();
    if (c == '"') {
      std::string ignored;
      return string(ignored);
    }
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      ++at_;
      space();
      if (take(close)) return true;
      for (;;) {
        if (c == '{') {
          std::string key;
          if (!string(key)) return false;
          space();
          if (!take(':')) return false;
        }
        if (!skip_value(depth + 1)) return false;
        space();
        if (take(close)) return true;
        if (!take(',')) return false;
        space();
      }
    }
    // A number, true, false or null.
    const std::size_t start = at_;
    while (at_ < text_.size() && (std::isalnum(static_cast<unsigned char>(text_[at_])) != 0 || text_[at_] == '-' ||
                                  text_[at_] == '+' || text_[at_] == '.'))
      ++at_;
    return at_ != start;
  }

  // A string, its escapes undone (\u as UTF-8, without joining surrogates).
  bool string(std::string& out) {
    if (!take('"')) return false;
    while (at_ < text_.size()) {
      const char c = text_[at_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (at_ >= text_.size()) return false;
      const char e = text_[at_++];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          if (at_ + 4 > text_.size()) return false;
          std::uint32_t code = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[at_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<std::uint32_t>(h - 'A' + 10);
            else return false;
          }
          if (code < 0x80) {
            out += static_cast<char>(code);
          } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: out += e; break;  // \" \\ \/
      }
    }
    return false;
  }

  void space() {
    while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])) != 0) ++at_;
  }
  char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
  bool take(char c) {
    if (peek() != c) return false;
    ++at_;
    return true;
  }

  std::string_view text_;
  std::size_t at_ = 0;
};

}  // namespace

std::optional<ReleaseVersion> parse_release_version(std::string_view text) {
  std::size_t at = 0;
  if (at < text.size() && (text[at] == 'v' || text[at] == 'V')) ++at;
  ReleaseVersion v;
  int* parts[3] = {&v.major, &v.minor, &v.patch};
  for (int i = 0; i < 3; ++i) {
    if (i > 0 && (at >= text.size() || text[at++] != '.')) return std::nullopt;
    const std::optional<int> n = number(text, at);
    if (!n) return std::nullopt;
    *parts[i] = *n;
  }
  if (text.substr(at, 5) == "-pre.") {
    at += 5;
    const std::optional<int> n = number(text, at);
    if (!n) return std::nullopt;
    v.pre = *n;
  }
  // The end, or the commit after a space: "0.1.0-pre.218 (abc1234)".
  if (at != text.size() && text[at] != ' ') return std::nullopt;
  return v;
}

bool newer(const ReleaseVersion& a, const ReleaseVersion& b) {
  if (a.major != b.major) return a.major > b.major;
  if (a.minor != b.minor) return a.minor > b.minor;
  if (a.patch != b.patch) return a.patch > b.patch;
  if (a.pre == b.pre) return false;
  if (a.pre < 0) return true;  // A release, after its pre-releases.
  if (b.pre < 0) return false;
  return a.pre > b.pre;
}

std::string version_text(const ReleaseVersion& v) {
  std::string text = "v" + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
  if (v.pre >= 0) text += "-pre." + std::to_string(v.pre);
  return text;
}

std::optional<std::vector<Release>> parse_releases(std::string_view json) { return Reader(json).releases(); }

UpdateResult compare_releases(std::string_view current_version, std::string_view json) {
  UpdateResult result;
  const std::optional<std::vector<Release>> releases = parse_releases(json);
  if (!releases) return result;
  std::optional<ReleaseVersion> best;
  for (const Release& r : *releases) {
    if (r.draft) continue;
    const std::optional<ReleaseVersion> v = parse_release_version(r.tag);
    if (!v || (best && !newer(*v, *best))) continue;
    best = v;
    result.latest = r;
  }
  if (!best) {
    result.kind = UpdateResult::NoRelease;
    return result;
  }
  if (result.latest.page.empty()) result.latest.page = std::string(kReleasesPage);
  const std::optional<ReleaseVersion> current = parse_release_version(current_version);
  if (!current) result.kind = UpdateResult::NotRelease;
  else result.kind = newer(*best, *current) ? UpdateResult::Newer : UpdateResult::UpToDate;
  return result;
}

}  // namespace bd::ui
