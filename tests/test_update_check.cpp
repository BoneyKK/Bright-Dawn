// The launcher's check for a newer version: versions read from the tags,
// compared, and the releases read from an answer shaped as GitHub's API's.
#include "test.h"
#include "ui/update_check.h"

using bd::ui::compare_releases;
using bd::ui::newer;
using bd::ui::parse_release_version;
using bd::ui::parse_releases;
using bd::ui::ReleaseVersion;
using bd::ui::UpdateResult;

namespace {

// Made up, as the API lists releases: newest first, with members the check
// does not read (an object, an array, numbers, null, escapes).
constexpr const char* kAnswer = R"([
  {
    "url": "https://api.github.com/repos/o/r/releases/3",
    "html_url": "https://github.com/o/r/releases/tag/v0.1.0-pre.12",
    "id": 3,
    "author": {"login": "someone", "id": 7, "site_admin": false, "tag_name": "v9.9.9"},
    "tag_name": "v0.1.0-pre.12",
    "name": "Bright Dawn \"v0.1.0-pre.12\" é",
    "draft": false,
    "prerelease": true,
    "assets": [{"name": "a.zip", "size": 1234, "download_count": 0}, {"name": "b.zip"}],
    "body": null
  },
  {"html_url": "https://github.com/o/r/releases/tag/v0.2.0", "tag_name": "v0.2.0", "draft": true},
  {"html_url": "https://github.com/o/r/releases/tag/v0.1.0-pre.9", "tag_name": "v0.1.0-pre.9", "draft": false},
  {"html_url": "https://github.com/o/r/releases/tag/nightly", "tag_name": "nightly", "draft": false}
])";

}  // namespace

TEST(update_check_reads_versions_as_the_tags_and_the_program_have_them) {
  CHECK((parse_release_version("v0.1.0-pre.218") == ReleaseVersion{0, 1, 0, 218}));
  CHECK((parse_release_version("0.1.0-pre.218 (abc1234)") == ReleaseVersion{0, 1, 0, 218}));
  CHECK((parse_release_version("v1.2.3") == ReleaseVersion{1, 2, 3, -1}));
  CHECK(!parse_release_version("0.1.0-dev"));
  CHECK(!parse_release_version("0.1.0-dev (abc1234)"));
  CHECK(!parse_release_version("nightly"));
  CHECK(!parse_release_version("v0.1"));
  CHECK(!parse_release_version("v0.1.0-pre."));
  CHECK(bd::ui::version_text(ReleaseVersion{0, 1, 0, 218}) == "v0.1.0-pre.218");
  CHECK(bd::ui::version_text(ReleaseVersion{1, 0, 0, -1}) == "v1.0.0");
}

TEST(update_check_puts_a_release_after_its_pre_releases) {
  const ReleaseVersion pre9{0, 1, 0, 9}, pre12{0, 1, 0, 12}, release{0, 1, 0, -1}, next{0, 1, 1, 1}, major{1, 0, 0, 1};
  CHECK(newer(pre12, pre9) && !newer(pre9, pre12));
  CHECK(newer(release, pre12) && !newer(pre12, release));
  CHECK(newer(next, release) && newer(major, next));
  CHECK(!newer(pre12, pre12) && !newer(release, release));
}

TEST(update_check_reads_the_releases_and_their_pages) {
  const auto releases = parse_releases(kAnswer);
  REQUIRE(releases.has_value());
  REQUIRE(releases->size() == 4);
  // The author's own "tag_name" is not the release's.
  CHECK((*releases)[0].tag == "v0.1.0-pre.12");
  CHECK((*releases)[0].page == "https://github.com/o/r/releases/tag/v0.1.0-pre.12");
  CHECK(!(*releases)[0].draft);
  CHECK((*releases)[1].draft);
  CHECK(parse_releases("[]").has_value() && parse_releases("[]")->empty());
  // Not a list of releases: an error from the API, cut short, or nothing.
  CHECK(!parse_releases(R"({"message": "API rate limit exceeded"})"));
  CHECK(!parse_releases(R"([{"tag_name": "v0.1.0-pre.1")"));
  CHECK(!parse_releases(""));
  CHECK(!parse_releases("[] trailing"));
}

TEST(update_check_finds_the_newest_release_against_this_version) {
  // A draft (v0.2.0) and a tag without a version do not count.
  UpdateResult r = compare_releases("0.1.0-pre.9 (abc1234)", kAnswer);
  CHECK(r.kind == UpdateResult::Newer);
  CHECK(r.latest.tag == "v0.1.0-pre.12");
  CHECK(r.latest.page == "https://github.com/o/r/releases/tag/v0.1.0-pre.12");
  CHECK(compare_releases("0.1.0-pre.12", kAnswer).kind == UpdateResult::UpToDate);
  CHECK(compare_releases("0.1.0-pre.13", kAnswer).kind == UpdateResult::UpToDate);
  CHECK(compare_releases("0.1.0-pre.200", kAnswer).kind == UpdateResult::UpToDate);
  // A build from the source tree is told the newest, never that it is newer.
  r = compare_releases("0.1.0-dev", kAnswer);
  CHECK(r.kind == UpdateResult::NotRelease);
  CHECK(r.latest.tag == "v0.1.0-pre.12");
  CHECK(compare_releases("0.1.0-pre.9", "[]").kind == UpdateResult::NoRelease);
  CHECK(compare_releases("0.1.0-pre.9", R"({"message": "Not Found"})").kind == UpdateResult::Unreadable);
}
