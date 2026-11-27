// Settings: what the player chose, kept between runs.
//
// The program describes each setting once (SettingDef): its key, the
// chapter of the settings book it is in, what it is called and what it does,
// its kind and the values it may take. Settings holds the values, checks
// every one against its description, and reads and writes them as a text
// file of "key = value" lines that a person can read:
//
//   # Window size: how many times the DS's size the window opens at.
//   # 1 to 8. Default: 2.
//   display.scale = 2
//
// A value read from the file that is not valid is reported and the default
// kept; lines of keys this build does not know are kept as they are, so that
// a newer build's settings survive an older one. The file is replaced
// whole, by writing a new one and renaming it over the old.
//
// A value can also be given for one run only (override), as the command line
// does: it is used, but not saved, until the setting is changed.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace bd::platform {

enum class SettingKind {
  Toggle,  // "on" or "off".
  Number,  // A whole number from `low` to `high`.
  Choice,  // One of `choices`, by value.
  Text,    // One line of text, such as a path.
};

struct SettingChoice {
  std::string value;  // As written in the file: lowercase, no spaces.
  std::string label;  // As shown.
};

struct SettingDef {
  std::string key;      // "chapter.name", lowercase.
  std::string chapter;  // The chapter of the book it is shown in; empty: not shown.
  std::string label;
  std::string help;     // A sentence or two: what it does.
  SettingKind kind = SettingKind::Toggle;
  std::string default_value;
  long long low = 0, high = 0;          // Number.
  std::vector<SettingChoice> choices;   // Choice.
  // Shown only while the toggle with this key is on (empty: always).
  std::string shown_with;
};

class Settings {
 public:
  // The definitions must have different keys and valid defaults.
  explicit Settings(std::vector<SettingDef> definitions);

  const std::vector<SettingDef>& definitions() const { return definitions_; }
  const SettingDef* find(std::string_view key) const;
  // The chapters with shown settings, in the order they first appear.
  std::vector<std::string> chapters() const;

  // The value in use, as text: the override, else the value set, else the
  // default. A key that is not defined gives an empty text.
  std::string value(std::string_view key) const;
  bool toggle(std::string_view key) const;
  long long number(std::string_view key) const;
  const std::string& choice(std::string_view key) const;
  bool is_default(std::string_view key) const;
  // Whether a setting is shown now (SettingDef::shown_with).
  bool shown(const SettingDef& def) const { return def.shown_with.empty() || toggle(def.shown_with); }

  // Sets a value, after checking it; ends an override of that key.
  Status set(std::string_view key, std::string_view value);
  // For this run only.
  Status set_for_run(std::string_view key, std::string_view value);
  bool overridden(std::string_view key) const;
  // Back to the default; ends an override.
  void reset(std::string_view key);
  void reset_all();

  // Goes up by one with every change of a value in use.
  std::uint64_t revision() const { return revision_; }

  // The file's text: every setting, with its description as comments, then
  // the lines kept of keys not known.
  std::string to_text() const;
  // Takes the values from a file's text. Returns what was wrong, a line
  // each; the rest is taken.
  std::vector<std::string> from_text(std::string_view text);

  // Reads the file. Its not being there is no problem: the defaults stay.
  // A file that cannot be read is an error; values that are not valid are
  // returned as problems, as by from_text.
  Result<std::vector<std::string>> load(const std::filesystem::path& path);
  Status save(const std::filesystem::path& path) const;

  // The value given in the form the kind takes, or nothing: "On", "yes",
  // "true" and "1" are "on", for example. For checking values from people.
  static std::optional<std::string> normalised(const SettingDef& def, std::string_view value);

 private:
  std::vector<SettingDef> definitions_;
  std::map<std::string, std::string, std::less<>> values_;     // Set, not default.
  std::map<std::string, std::string, std::less<>> overrides_;
  std::vector<std::string> unknown_lines_;
  std::uint64_t revision_ = 0;
};

}  // namespace bd::platform
