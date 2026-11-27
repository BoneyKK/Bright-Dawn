// Whether a newer Bright Dawn is out: the releases on GitHub, as its API
// lists them, against this program's version. The launcher fetches the list
// (app/update_check.h); what is read from it, and how versions compare, is
// here, so that it can be tested without the network.
//
// Versions are as the release tags have them: "v0.1.0-pre.218" for a
// pre-release (the release job's run number), "v0.1.0" for a release. A
// release is newer than every pre-release of its version. A program built
// from the source tree ("0.1.0-dev") is no release: it is never told that a
// release is newer.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd::ui {

// The releases: on GitHub, and as its API lists them (newest first, the
// pre-releases too, which "latest" leaves out).
inline constexpr std::string_view kReleasesPage = "https://github.com/BoneyKK/Bright-Dawn/releases";
inline constexpr std::string_view kReleasesApi = "https://api.github.com/repos/BoneyKK/Bright-Dawn/releases?per_page=10";

struct ReleaseVersion {
  int major = 0, minor = 0, patch = 0;
  int pre = -1;  // The pre-release's number; -1 for a release.

  friend bool operator==(const ReleaseVersion&, const ReleaseVersion&) = default;
};

// "v0.1.0-pre.218", "0.1.0-pre.218 (abc1234)", "v0.1.0": the version; for
// anything else (a build from the source tree, "0.1.0-dev"), nothing.
std::optional<ReleaseVersion> parse_release_version(std::string_view text);
// Whether `a` is newer than `b`.
bool newer(const ReleaseVersion& a, const ReleaseVersion& b);
std::string version_text(const ReleaseVersion& v);  // As a tag has it: "v0.1.0-pre.218".

struct Release {
  std::string tag;   // "v0.1.0-pre.218".
  std::string page;  // Its page on GitHub.
  bool draft = false;
};

// The releases in the API's answer (a JSON array of objects), in its order.
// Nothing if it is not such an array.
std::optional<std::vector<Release>> parse_releases(std::string_view json);

struct UpdateResult {
  enum Kind {
    Unreadable,  // The answer is not a list of releases.
    NoRelease,   // No release with a version.
    UpToDate,    // None newer than this program.
    Newer,       // `latest` is newer than this program.
    NotRelease,  // This program is no release (a build from the source tree): `latest` is the newest.
  } kind = Unreadable;
  Release latest;  // The newest release, with a version, not a draft.
};
UpdateResult compare_releases(std::string_view current_version, std::string_view json);

}  // namespace bd::ui
