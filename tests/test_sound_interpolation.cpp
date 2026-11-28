// Interpolation of sampled sound between its samples, as melonDS does it:
// the levels its formulas give, where the timer is between two samples,
// and the settings' names.
#include <array>
#include <cstdint>
#include <cstdlib>

#include "nds/sound_interpolation.h"
#include "test.h"

using bd::nds::Interpolation;
using bd::nds::interpolate;

namespace {

// Oldest 1000, then 2000 and 3000, and 4000 now.
constexpr std::array<std::int32_t, 3> kRising = {3000, 2000, 1000};

}  // namespace

TEST(sound_interpolation_gives_melonds_levels) {
  // None: the sample now, as the DS holds it.
  CHECK(interpolate(Interpolation::kNone, 4000, kRising, 100) == 4000);
  // Linear: (now * pos + before * (255 - pos)) >> 8, as melonDS has it.
  CHECK(interpolate(Interpolation::kLinear, 4000, kRising, 0) == (3000 * 255) >> 8);
  CHECK(interpolate(Interpolation::kLinear, 4000, kRising, 255) == (4000 * 255) >> 8);
  CHECK(interpolate(Interpolation::kLinear, 4000, kRising, 128) == (4000 * 128 + 3000 * 127) >> 8);
  // Cosine: from the sample before to the one now, flat at both ends.
  CHECK(interpolate(Interpolation::kCosine, 4000, kRising, 0) == 3000);
  CHECK(interpolate(Interpolation::kCosine, 4000, kRising, 255) == 4000);
  const std::int32_t middle = interpolate(Interpolation::kCosine, 4000, kRising, 128);
  CHECK(middle > 3450 && middle < 3550);
  // Cubic: from [1] to [0], through the four. At 128 the coefficients are
  // -2048, 10240, 10240 and -2048 (out of 16384).
  CHECK(interpolate(Interpolation::kCubic, 4000, kRising, 0) == 2000);
  CHECK(interpolate(Interpolation::kCubic, 4000, kRising, 128) ==
        (1000 * -2048 + 2000 * 10240 + 3000 * 10240 + 4000 * -2048) >> 14);
  // Gaussian: a level held steady stays where it is, near enough (the SNES
  // table's four weights add up to about one), and the loudest levels do
  // not wrap.
  const std::array<std::int32_t, 3> steady = {8000, 8000, 8000};
  for (std::int32_t pos : {0, 64, 128, 255}) CHECK(std::abs(interpolate(Interpolation::kGaussian, 8000, steady, pos) - 8000) < 80);
  const std::array<std::int32_t, 3> loud = {0x7FFF, 0x7FFF, 0x7FFF};
  CHECK(interpolate(Interpolation::kGaussian, 0x7FFF, loud, 128) > 0x7000);
  // A position out of range is taken as its end.
  CHECK(interpolate(Interpolation::kCosine, 4000, kRising, 400) == 4000);
}

TEST(sound_interpolation_position_is_where_the_timer_is_between_samples) {
  CHECK(bd::nds::interpolation_position(0xC000, 0xC000) == 0);
  CHECK(bd::nds::interpolation_position(0xE000, 0xC000) == 128);
  CHECK(bd::nds::interpolation_position(0xFFFF, 0xC000) == 255);
  CHECK(bd::nds::interpolation_position(0x1000, 0xC000) == 0);  // Before the reload value.
  CHECK(bd::nds::interpolation_position(0x8000, 0x10000) == 0);
}

TEST(sound_interpolation_names_are_the_settings_values) {
  for (Interpolation mode : {Interpolation::kNone, Interpolation::kLinear, Interpolation::kCosine,
                             Interpolation::kCubic, Interpolation::kGaussian})
    CHECK(bd::nds::interpolation_named(bd::nds::interpolation_name(mode)) == mode);
  CHECK(bd::nds::interpolation_name(Interpolation::kGaussian) == "gaussian");
  CHECK(!bd::nds::interpolation_named("sinc").has_value());
}
