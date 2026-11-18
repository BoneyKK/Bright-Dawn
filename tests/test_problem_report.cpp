// The address "Report a problem" opens.
#include <string>

#include "test.h"
#include "ui/problem_report.h"

using bd::ui::percent_encode;
using bd::ui::problem_report_url;
using bd::ui::ProblemReport;

TEST(percent_encode_keeps_only_unreserved_characters) {
  CHECK(percent_encode("Az09-._~") == "Az09-._~");
  CHECK(percent_encode("a b&c=d?e/f#g+h%") == "a%20b%26c%3Dd%3Fe%2Ff%23g%2Bh%25");
  CHECK(percent_encode("\n") == "%0A");
  CHECK(percent_encode("\xC3\xA9") == "%C3%A9");  // é, byte by byte.
  CHECK(percent_encode("").empty());
}

TEST(problem_report_url_opens_the_bug_form) {
  CHECK(problem_report_url({}) == "https://github.com/BoneyKK/Bright-Dawn/issues/new?template=bug.yml");
}

TEST(problem_report_url_fills_in_what_is_known) {
  ProblemReport report;
  report.version = "0.1.0-dev (abc1234)";
  report.os = "Windows";
  report.renderer = "vulkan";
  report.game_code = "BO5P";
  report.code = "The built game";
  CHECK(problem_report_url(report) ==
        "https://github.com/BoneyKK/Bright-Dawn/issues/new?template=bug.yml"
        "&version=0.1.0-dev%20%28abc1234%29&os=Windows&renderer=vulkan&game-code=BO5P&code=The%20built%20game");
  // A field not known is left for the player, not sent empty.
  report.renderer.clear();
  report.game_code.clear();
  CHECK(problem_report_url(report) ==
        "https://github.com/BoneyKK/Bright-Dawn/issues/new?template=bug.yml"
        "&version=0.1.0-dev%20%28abc1234%29&os=Windows&code=The%20built%20game");
}
