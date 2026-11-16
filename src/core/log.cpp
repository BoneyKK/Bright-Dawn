#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <system_error>
#include <tuple>

#if defined(_WIN32)
#include <io.h>
#endif

namespace bd::log {
namespace {

struct State {
  std::mutex mutex;
  Level level = Level::Info;
  std::FILE* file = nullptr;
  std::filesystem::path path;
  bool standard_error = false;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

State& state() {
  static State s;
  return s;
}

std::tm local_time(std::time_t t) {
  std::tm out{};
#if defined(_WIN32)
  localtime_s(&out, &t);
#else
  localtime_r(&t, &out);
#endif
  return out;
}

std::string wall_clock_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  const std::tm t = local_time(seconds);
  char text[96];
  std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d.%03d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec, static_cast<int>(ms < 0 ? ms + 1000 : ms));
  return text;
}

std::FILE* open_for_writing(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  return _wfopen_s(&file, path.c_str(), L"wb") == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

// A log file's name: `prefix`, a time, perhaps a number, ".log". Returns
// what it sorts by (the time, then the number), or nothing for another name.
std::optional<std::tuple<std::string, unsigned long>> log_file_key(const std::string& name, std::string_view prefix) {
  constexpr std::string_view kEnd = ".log";
  constexpr std::size_t kTimeLength = 15;  // YYYYMMDD-HHMMSS
  if (name.size() < prefix.size() + kTimeLength + kEnd.size()) return std::nullopt;
  if (name.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
  if (name.compare(name.size() - kEnd.size(), kEnd.size(), kEnd) != 0) return std::nullopt;
  const std::string time = name.substr(prefix.size(), kTimeLength);
  for (std::size_t i = 0; i < kTimeLength; ++i) {
    const bool digit = time[i] >= '0' && time[i] <= '9';
    if (i == 8 ? time[i] != '-' : !digit) return std::nullopt;
  }
  const std::string rest = name.substr(prefix.size() + kTimeLength, name.size() - prefix.size() - kTimeLength - kEnd.size());
  unsigned long number = 1;
  if (!rest.empty()) {
    if (rest.size() < 2 || rest[0] != '-' || rest.size() > 6) return std::nullopt;
    number = 0;
    for (std::size_t i = 1; i < rest.size(); ++i) {
      if (rest[i] < '0' || rest[i] > '9') return std::nullopt;
      number = number * 10 + static_cast<unsigned long>(rest[i] - '0');
    }
  }
  return std::make_tuple(time, number);
}

}  // namespace

const char* level_name(Level level) {
  switch (level) {
    case Level::Error: return "error";
    case Level::Warning: return "warning";
    case Level::Info: return "info";
    case Level::Debug: return "debug";
  }
  return "info";
}

std::optional<Level> level_named(std::string_view name) {
  for (Level level : {Level::Error, Level::Warning, Level::Info, Level::Debug})
    if (name == level_name(level)) return level;
  return std::nullopt;
}

void set_level(Level level) {
  std::lock_guard lock(state().mutex);
  state().level = level;
}

Level level() {
  std::lock_guard lock(state().mutex);
  return state().level;
}

bool enabled(Level level) {
  std::lock_guard lock(state().mutex);
  return static_cast<int>(level) <= static_cast<int>(state().level);
}

Status open_file(const std::filesystem::path& path) {
  std::FILE* file = open_for_writing(path);
  if (file == nullptr) return Error{ErrorCode::Io, "cannot write the log file " + path.string()};
  std::lock_guard lock(state().mutex);
  if (state().file != nullptr) std::fclose(state().file);
  state().file = file;
  state().path = path;
  return {};
}

std::filesystem::path file_path() {
  std::lock_guard lock(state().mutex);
  return state().path;
}

void close_file() {
  std::lock_guard lock(state().mutex);
  if (state().file != nullptr) std::fclose(state().file);
  state().file = nullptr;
  state().path.clear();
}

void set_standard_error(bool on) {
  std::lock_guard lock(state().mutex);
  state().standard_error = on;
}

std::string format_line(std::string_view wall_clock, std::uint64_t since_start, Level level, std::string_view area,
                        std::string_view message) {
  char head[96];
  const char* names[] = {"ERROR", "WARN ", "INFO ", "DEBUG"};
  std::snprintf(head, sizeof head, "%.*s %4llu.%03llu %s %-8.*s ", static_cast<int>(wall_clock.size()),
                wall_clock.data(), static_cast<unsigned long long>(since_start / 1000),
                static_cast<unsigned long long>(since_start % 1000), names[static_cast<int>(level)],
                static_cast<int>(area.size()), area.data());
  std::string line = head;
  const std::size_t indent = line.size();
  // Later lines of the message under the first; a line end at the very end
  // is dropped.
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.remove_suffix(1);
  for (std::size_t i = 0; i < message.size(); ++i) {
    const char c = message[i];
    if (c == '\r') continue;
    if (c == '\n') {
      line += '\n';
      line.append(indent, ' ');
    } else {
      line += c;
    }
  }
  return line;
}

void write(Level level, std::string_view area, std::string_view message) {
  State& s = state();
  std::lock_guard lock(s.mutex);
  if (static_cast<int>(level) > static_cast<int>(s.level)) return;
  if (s.file == nullptr && !s.standard_error) return;
  const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.start);
  std::string line = format_line(wall_clock_now(), static_cast<std::uint64_t>(since.count()), level, area, message);
  line += '\n';
  if (s.file != nullptr) {
    std::fwrite(line.data(), 1, line.size(), s.file);
    std::fflush(s.file);
  }
  if (s.standard_error) {
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
  }
}

void writef(Level level, const char* area, const char* format, ...) {
  if (!enabled(level)) return;
  std::va_list arguments;
  va_start(arguments, format);
  std::va_list copy;
  va_copy(copy, arguments);
  const int length = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  std::string message;
  if (length > 0) {
    message.resize(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(message.data(), message.size(), format, arguments);
    message.resize(static_cast<std::size_t>(length));
  }
  va_end(arguments);
  write(level, area, message);
}

void error(std::string_view area, std::string_view message) { write(Level::Error, area, message); }
void warning(std::string_view area, std::string_view message) { write(Level::Warning, area, message); }
void info(std::string_view area, std::string_view message) { write(Level::Info, area, message); }
void debug(std::string_view area, std::string_view message) { write(Level::Debug, area, message); }

int file_descriptor() {
  std::lock_guard lock(state().mutex);
  if (state().file == nullptr) return -1;
#if defined(_WIN32)
  return _fileno(state().file);
#else
  return fileno(state().file);
#endif
}

std::filesystem::path new_file_name(const std::filesystem::path& folder, std::string_view prefix) {
  const std::tm t = local_time(std::time(nullptr));
  char time[96];
  std::snprintf(time, sizeof time, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                t.tm_min, t.tm_sec);
  const std::string base = std::string(prefix) + time;
  std::error_code ec;
  std::filesystem::path path = folder / (base + ".log");
  for (int n = 2; std::filesystem::exists(path, ec) && n < 100000; ++n)
    path = folder / (base + "-" + std::to_string(n) + ".log");
  return path;
}

std::size_t remove_old_files(const std::filesystem::path& folder, std::string_view prefix, std::size_t keep) {
  std::error_code ec;
  std::vector<std::pair<std::tuple<std::string, unsigned long>, std::filesystem::path>> found;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) continue;
    const std::filesystem::path& p = entry.path();
    const std::u8string name8 = p.filename().u8string();
    const std::string name(name8.begin(), name8.end());
    if (const auto key = log_file_key(name, prefix)) found.emplace_back(*key, p);
  }
  if (found.size() <= keep) return 0;
  std::sort(found.begin(), found.end());
  const std::filesystem::path open = file_path();
  std::size_t removed = 0;
  for (std::size_t i = 0; i + keep < found.size(); ++i) {
    if (!open.empty() && std::filesystem::equivalent(found[i].second, open, ec)) continue;
    if (std::filesystem::remove(found[i].second, ec)) ++removed;
  }
  return removed;
}

}  // namespace bd::log
