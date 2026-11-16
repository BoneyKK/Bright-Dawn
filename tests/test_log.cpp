// The log: its lines, its levels, its files.
#include <string>

#include "core/log.h"
#include "temp_dir.h"
#include "test.h"

namespace {

using bd::log::Level;
using bd_test::TempDir;

// The log is the program's one log: each test puts back what it changed.
struct Restore {
  Level level = bd::log::level();
  ~Restore() {
    bd::log::close_file();
    bd::log::set_level(level);
  }
};

}  // namespace

TEST(log_levels_have_names) {
  for (Level level : {Level::Error, Level::Warning, Level::Info, Level::Debug})
    CHECK(bd::log::level_named(bd::log::level_name(level)) == level);
  CHECK(std::string(bd::log::level_name(Level::Warning)) == "warning");
  CHECK(!bd::log::level_named("loud").has_value() && !bd::log::level_named("").has_value());
}

TEST(log_lines_carry_the_time_the_level_and_the_area) {
  CHECK(bd::log::format_line("2026-11-16 14:03:22.517", 12345, Level::Info, "window", "Full screen on") ==
        "2026-11-16 14:03:22.517   12.345 INFO  window   Full screen on");
  CHECK(bd::log::format_line("2026-11-16 14:03:22.517", 7, Level::Error, "rom", "x") ==
        "2026-11-16 14:03:22.517    0.007 ERROR rom      x");
  // Later lines under the first; a line end at the end is dropped, and
  // carriage returns are.
  const std::string head = "2026-11-16 14:03:22.517    1.000 WARN  game     ";
  CHECK(bd::log::format_line("2026-11-16 14:03:22.517", 1000, Level::Warning, "game", "one\r\ntwo\n") ==
        head + "one\n" + std::string(head.size(), ' ') + "two");
}

TEST(log_writes_lines_of_the_level_and_above_to_its_file) {
  Restore restore;
  TempDir dir;
  REQUIRE(bd::log::open_file(dir.path() / "a.log").ok());
  CHECK(bd::log::file_path() == dir.path() / "a.log");
  CHECK(bd::log::file_descriptor() >= 0);
  bd::log::set_level(Level::Info);
  CHECK(bd::log::enabled(Level::Warning) && bd::log::enabled(Level::Info) && !bd::log::enabled(Level::Debug));
  bd::log::info("test", "first");
  bd::log::debug("test", "not written");
  bd::log::writef(Level::Error, "test", "number %d, text %s", 42, "here");
  // Written out at once: the file has the lines while it is still open.
  const std::string text = dir.read("a.log");
  CHECK(text.find("INFO  test     first\n") != std::string::npos);
  CHECK(text.find("not written") == std::string::npos);
  CHECK(text.find("ERROR test     number 42, text here\n") != std::string::npos);
  CHECK(text.find("first") < text.find("number 42"));
  bd::log::close_file();
  CHECK(bd::log::file_path().empty() && bd::log::file_descriptor() == -1);
  bd::log::info("test", "after closing");
  CHECK(dir.read("a.log") == text);
  // A file that cannot be written is an error, not a crash.
  CHECK(!bd::log::open_file(dir.path() / "no such folder" / "b.log").ok());
}

TEST(log_files_get_new_names_and_old_ones_are_removed) {
  Restore restore;
  TempDir dir;
  const std::filesystem::path first = bd::log::new_file_name(dir.path(), "bright-dawn-");
  const std::string name = first.filename().string();
  // bright-dawn-YYYYMMDD-HHMMSS.log
  CHECK(name.size() == std::string("bright-dawn-20261116-140322.log").size());
  CHECK(name.rfind("bright-dawn-", 0) == 0 && name.substr(name.size() - 4) == ".log");
  dir.write(name, "");
  // The same second again: a number is added.
  const std::filesystem::path second = bd::log::new_file_name(dir.path(), "bright-dawn-");
  CHECK(second != first && second.filename().string().find("-2.log") != std::string::npos);

  for (const char* n : {"bright-dawn-20250101-000000.log", "bright-dawn-20250101-000000-2.log",
                        "bright-dawn-20250101-000000-10.log", "bright-dawn-20240101-120000.log",
                        "bright-dawn-20260101-120000.log"})
    dir.write(n, "");
  // Not log files of this name: left alone.
  for (const char* n : {"bright-dawn-notes.log", "other-20200101-000000.log", "bright-dawn-20200101-000000.txt"})
    dir.write(n, "");
  CHECK(bd::log::remove_old_files(dir.path(), "bright-dawn-", 3) == 3);
  const auto there = [&](const char* n) { return std::filesystem::exists(dir.path() / n); };
  // The oldest go: by time, then by number (2 before 10).
  CHECK(!there("bright-dawn-20240101-120000.log") && !there("bright-dawn-20250101-000000.log") &&
        !there("bright-dawn-20250101-000000-2.log"));
  CHECK(there("bright-dawn-20250101-000000-10.log") && there("bright-dawn-20260101-120000.log") && there(name.c_str()));
  CHECK(there("bright-dawn-notes.log") && there("other-20200101-000000.log") && there("bright-dawn-20200101-000000.txt"));
  CHECK(bd::log::remove_old_files(dir.path(), "bright-dawn-", 3) == 0);
}
