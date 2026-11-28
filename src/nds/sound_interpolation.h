// Interpolation of the DS's sampled sound between its samples, for the
// speakers only (Settings, Sound, Interpolation).
//
// The DS holds each sample of a sound until the next one; the steps between
// them are heard as a hiss, and most of all in sounds made at a low rate.
// These ways of going smoothly from one sample to the next are melonDS's,
// as its sound unit has them (melonDS src/SPU.cpp), and give the same
// levels as melonDS does from the same samples. The DS itself does none of
// them: the console model's 10-bit output, its capture units and its
// registers are not changed by them (nds/sound_hardware.h).
//
// Adapted from melonDS, commit b86390e4428bf38ce4c1ce0e9ca446d6d25955e8
// (2025-11-18), https://melonds.kuribo64.net:
//
//   Copyright 2016-2025 melonDS team
//
//   melonDS is free software: you can redistribute it and/or modify it under
//   the terms of the GNU General Public License as published by the Free
//   Software Foundation, either version 3 of the License, or (at your option)
//   any later version.
//
// Bright Dawn uses it under the GPL-3.0, its own licence (LICENSE).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bd::nds {

enum class Interpolation : std::uint8_t {
  kNone,      // Each sample held until the next, as the DS does.
  kLinear,    // A straight line from the sample before to this one.
  kCosine,    // A half cosine between the two: no corner at each sample.
  kCubic,     // A cubic through the last four samples.
  kGaussian,  // The SNES sound unit's Gaussian filter over the last four.
};

// The settings' names: none, linear, cosine, cubic, gaussian.
std::string_view interpolation_name(Interpolation mode);
std::optional<Interpolation> interpolation_named(std::string_view name);

// How far a channel's timer is through its present sample, from 0 to 255:
// `counter` the timer's count, `reload` the value it starts from.
std::int32_t interpolation_position(std::uint32_t counter, std::uint32_t reload);

// The level between samples: `now` the present sample, `previous` the ones
// before it, newest first ([0] the sample just before `now`), `position`
// from interpolation_position(). Levels are signed 16-bit. With kNone, `now`.
// The level is between the sample before `now` and `now` (for kCubic and
// kGaussian, between [1] and [0]): it is one or two samples late, as in
// melonDS.
std::int32_t interpolate(Interpolation mode, std::int32_t now, const std::array<std::int32_t, 3>& previous,
                         std::int32_t position);

}  // namespace bd::nds
