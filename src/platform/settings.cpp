#include "platform/settings.h"

#include <cassert>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace bd::platform {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::optional<long long> whole_number(std::string_view s) {
  bool negative = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    negative = s.front() == '-';
    s.remove_prefix(1);
  }
  if (s.empty() || s.size() > 18) return std::nullopt;
  long long value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
  }
  return negative ? -value : value;
}

// What a definition allows, said in a few words, for the file and for
// problems.
std::string allowed(const SettingDef& def) {
  switch (def.kind) {
    case SettingKind::Toggle: return "on or off";
    case SettingKind::Number: return std::to_string(def.low) + " to " + std::to_string(def.high);
    case SettingKind::Choice: {
      std::string text = "one of";
      for (std::size_t i = 0; i < def.choices.size(); ++i)
        text += (i == 0 ? " " : ", ") + def.choices[i].value;
      return text;
    }
    case SettingKind::Text: return "one line of text";
  }
  return "";
}

// Lines of at most about 76 characters, each starting with "# ".
void comment(std::string& out, std::string_view text) {
  std::string line;
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t end = text.find(' ', at);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view word = text.substr(at, end - at);
    if (!line.empty() && line.size() + 1 + word.size() > 76) {
      out += "# " + line + "\n";
      line.clear();
    }
    if (!word.empty()) {
      if (!line.empty()) line += ' ';
      line += word;
    }
    at = end + 1;
  }
  if (!line.empty()) out += "# " + line + "\n";
}

}  // namespace

std::optional<std::string> Settings::normalised(const SettingDef& def, std::string_view value) {
  value = trim(value);
  switch (def.kind) {
    case SettingKind::Toggle: {
      const std::string v = lower(value);
      if (v == "on" || v == "yes" || v == "true" || v == "1") return std::string("on");
      if (v == "off" || v == "no" || v == "false" || v == "0") return std::string("off");
      return std::nullopt;
    }
    case SettingKind::Number: {
      const auto n = whole_number(value);
      if (!n || *n < def.low || *n > def.high) return std::nullopt;
      return std::to_string(*n);
    }
    case SettingKind::Choice: {
      const std::string v = lower(value);
      for (const SettingChoice& c : def.choices)
        if (c.value == v) return c.value;
      return std::nullopt;
    }
    case SettingKind::Text:
      for (char c : value)
        if (c == '\n' || c == '\r' || c == '\0') return std::nullopt;
      return std::string(value);
  }
  return std::nullopt;
}

Settings::Settings(std::vector<SettingDef> definitions) : definitions_(std::move(definitions)) {
#ifndef NDEBUG
  std::set<std::string> keys;
  for (const SettingDef& def : definitions_) {
    assert(keys.insert(def.key).second && "two settings with one key");
    assert(normalised(def, def.default_value) == def.default_value && "a default that is not valid");
  }
#endif
}

const SettingDef* Settings::find(std::string_view key) const {
  for (const SettingDef& def : definitions_)
    if (def.key == key) return &def;
  return nullptr;
}

std::vector<std::string> Settings::chapters() const {
  std::vector<std::string> out;
  for (const SettingDef& def : definitions_) {
    if (def.chapter.empty()) continue;
    bool seen = false;
    for (const std::string& c : out) seen = seen || c == def.chapter;
    if (!seen) out.push_back(def.chapter);
  }
  return out;
}

std::string Settings::value(std::string_view key) const {
  if (auto it = overrides_.find(key); it != overrides_.end()) return it->second;
  if (auto it = values_.find(key); it != values_.end()) return it->second;
  const SettingDef* def = find(key);
  return def != nullptr ? def->default_value : std::string();
}

bool Settings::toggle(std::string_view key) const {
  assert(find(key) != nullptr && find(key)->kind == SettingKind::Toggle);
  return value(key) == "on";
}

long long Settings::number(std::string_view key) const {
  assert(find(key) != nullptr && find(key)->kind == SettingKind::Number);
  return whole_number(value(key)).value_or(0);
}

const std::string& Settings::choice(std::string_view key) const {
  const SettingDef* def = find(key);
  assert(def != nullptr && def->kind == SettingKind::Choice);
  static const std::string kNone;
  if (def == nullptr) return kNone;
  if (auto it = overrides_.find(key); it != overrides_.end()) return it->second;
  if (auto it = values_.find(key); it != values_.end()) return it->second;
  return def->default_value;
}

bool Settings::is_default(std::string_view key) const {
  const SettingDef* def = find(key);
  return def != nullptr && value(key) == def->default_value;
}

Status Settings::set(std::string_view key, std::string_view value_text) {
  const SettingDef* def = find(key);
  if (def == nullptr) return Error{ErrorCode::InvalidArgument, "there is no setting " + std::string(key)};
  const auto v = normalised(*def, value_text);
  if (!v)
    return Error{ErrorCode::InvalidArgument,
                 def->label + ": \"" + std::string(trim(value_text)) + "\" is not " + allowed(*def)};
  const std::string before = value(key);
  values_[def->key] = *v;
  overrides_.erase(def->key);
  if (value(key) != before) ++revision_;
  return {};
}

Status Settings::set_for_run(std::string_view key, std::string_view value_text) {
  const SettingDef* def = find(key);
  if (def == nullptr) return Error{ErrorCode::InvalidArgument, "there is no setting " + std::string(key)};
  const auto v = normalised(*def, value_text);
  if (!v)
    return Error{ErrorCode::InvalidArgument,
                 def->label + ": \"" + std::string(trim(value_text)) + "\" is not " + allowed(*def)};
  const std::string before = value(key);
  overrides_[def->key] = *v;
  if (value(key) != before) ++revision_;
  return {};
}

bool Settings::overridden(std::string_view key) const { return overrides_.find(key) != overrides_.end(); }

void Settings::reset(std::string_view key) {
  const SettingDef* def = find(key);
  if (def == nullptr) return;
  const std::string before = value(key);
  values_.erase(def->key);
  overrides_.erase(def->key);
  if (value(key) != before) ++revision_;
}

void Settings::reset_all() {
  for (const SettingDef& def : definitions_) reset(def.key);
}

std::string Settings::to_text() const {
  std::string out;
  comment(out,
          "Bright Dawn's settings, as chosen in the program. The file may be edited while the program is not "
          "running: a line \"key = value\" for each setting; lines that start with # are comments.");
  for (const SettingDef& def : definitions_) {
    out += "\n";
    comment(out, (def.chapter.empty() ? "" : def.chapter + ", ") + def.label + ": " + def.help);
    std::string range = allowed(def);
    if (!range.empty() && range[0] >= 'a' && range[0] <= 'z') range[0] = static_cast<char>(range[0] - 'a' + 'A');
    comment(out, range + ". Default: " + (def.default_value.empty() ? "none" : def.default_value) + ".");
    const auto it = values_.find(def.key);
    out += def.key + " = " + (it != values_.end() ? it->second : def.default_value) + "\n";
  }
  if (!unknown_lines_.empty()) {
    out += "\n";
    comment(out, "Not known to this version of Bright Dawn, kept as they were:");
    for (const std::string& line : unknown_lines_) out += line + "\n";
  }
  return out;
}

std::vector<std::string> Settings::from_text(std::string_view text) {
  std::vector<std::string> problems;
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
  unknown_lines_.clear();
  std::size_t number = 0;
  while (!text.empty()) {
    ++number;
    std::size_t end = text.find('\n');
    if (end == std::string_view::npos) end = text.size();
    const std::string_view raw = text.substr(0, end);
    text.remove_prefix(end == text.size() ? end : end + 1);
    const std::string_view line = trim(raw);
    if (line.empty() || line.front() == '#') continue;
    const std::string where = "line " + std::to_string(number) + ": ";
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      problems.push_back(where + "not \"key = value\": " + std::string(line));
      continue;
    }
    const std::string key = lower(trim(line.substr(0, equals)));
    const std::string_view value_text = trim(line.substr(equals + 1));
    const SettingDef* def = find(key);
    if (def == nullptr) {
      unknown_lines_.emplace_back(line);
      continue;
    }
    if (Status s = set(key, value_text); !s)
      problems.push_back(where + s.error().message + "; " +
                         (def->default_value.empty() ? std::string("it is left empty")
                                                     : "the default, " + def->default_value + ", is kept"));
  }
  return problems;
}

Result<std::vector<std::string>> Settings::load(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return std::vector<std::string>{};
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return Error{ErrorCode::Io, "cannot read the settings file " + path.string() + ": " + ec.message()};
  if (size > 1024 * 1024) return Error{ErrorCode::Corrupt, "the settings file " + path.string() + " is too large"};
  std::ifstream in(path, std::ios::binary);
  if (!in) return Error{ErrorCode::Io, "cannot read the settings file " + path.string()};
  std::ostringstream text;
  text << in.rdbuf();
  return from_text(text.str());
}

Status Settings::save(const std::filesystem::path& path) const {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path temporary = path;
  temporary += ".new";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::Io, "cannot write the settings file " + temporary.string()};
    const std::string text = to_text();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) return Error{ErrorCode::Io, "cannot write the settings file " + temporary.string()};
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return Error{ErrorCode::Io, "cannot replace the settings file " + path.string()};
  }
  return {};
}

}  // namespace bd::platform
