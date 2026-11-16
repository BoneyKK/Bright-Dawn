// The log: a record of what happened during a run, so that a problem a
// player meets can be traced afterwards.
//
// Every line carries the wall-clock time, the time since the log was
// started, a level and the part of the program it comes from:
//
//   2026-11-16 14:03:22.517  +12.345 INFO  window   Full screen on
//
// Lines go to a file (open_file) and, if asked, to the standard error
// stream. Each line is written out as soon as it is logged, so that a log
// ends with what happened last even when the program crashes; the levels
// keep the amount small enough for that. A message of several lines is
// written with its later lines indented under the first.
//
// Logging may be used from any thread. Until a file is opened, or the
// standard error stream is asked for, nothing is written.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

#if defined(__GNUC__) || defined(__clang__)
#define BD_PRINTF_LIKE(format_index, first_argument) \
  __attribute__((format(printf, format_index, first_argument)))
#else
#define BD_PRINTF_LIKE(format_index, first_argument)
#endif

namespace bd::log {

enum class Level {
  Error,    // Something failed: what the player was told, and why.
  Warning,  // Something is not as it should be, but the run goes on.
  Info,     // What the program did: the default.
  Debug,    // In more detail, for tracing a problem.
};

// "error", "warning", "info", "debug".
const char* level_name(Level level);
// The level of that name, or nothing.
std::optional<Level> level_named(std::string_view name);

// Lines of this level and the more important ones are written; the others
// are not. The default is Info.
void set_level(Level level);
Level level();
bool enabled(Level level);

// Starts writing to `path`, replacing what is there; a file already open is
// closed first.
Status open_file(const std::filesystem::path& path);
// The file being written, or an empty path.
std::filesystem::path file_path();
// Stops writing to the file.
void close_file();
// Whether lines are also written to the standard error stream.
void set_standard_error(bool on);

// Writes a line. `area` names the part of the program, in a few lowercase
// letters ("window", "game", "rom", ...).
void write(Level level, std::string_view area, std::string_view message);
// The same, with the message made as printf makes it.
void writef(Level level, const char* area, const char* format, ...) BD_PRINTF_LIKE(3, 4);

void error(std::string_view area, std::string_view message);
void warning(std::string_view area, std::string_view message);
void info(std::string_view area, std::string_view message);
void debug(std::string_view area, std::string_view message);

// The line as written, without its end: for tests, and for the file.
// `since_start` is in milliseconds; `wall_clock` is "YYYY-MM-DD HH:MM:SS.mmm".
std::string format_line(std::string_view wall_clock, std::uint64_t since_start, Level level, std::string_view area,
                        std::string_view message);

// The operating system's handle of the open file, or -1: for a crash
// handler, which may only write with it. A line written by it should
// begin and end with a line end of its own.
int file_descriptor();

// Log files in `folder` named `prefix`YYYYMMDD-HHMMSS[-N].log: a new name,
// unused, for a log started at the current local time.
std::filesystem::path new_file_name(const std::filesystem::path& folder, std::string_view prefix);
// Removes the oldest of those files (by name, which is by time) until at
// most `keep` are left. Returns how many were removed.
std::size_t remove_old_files(const std::filesystem::path& folder, std::string_view prefix, std::size_t keep);

}  // namespace bd::log
