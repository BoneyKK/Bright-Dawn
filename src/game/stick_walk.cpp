#include "game/stick_walk.h"

#include <algorithm>
#include <cmath>

namespace bd::game {

namespace {

constexpr std::uint32_t kMainMemory = OpeningSkip::kMainMemory, kMainMemorySize = OpeningSkip::kMainMemorySize;
constexpr double kPi = 3.14159265358979323846;

bool in_main_memory(std::uint32_t address) {
  return address >= kMainMemory && address < kMainMemory + kMainMemorySize && (address & 3) == 0;
}

std::uint32_t read16(const GuestMemory& memory, std::uint32_t address) {
  return (memory.read32(address & ~3u) >> ((address & 2) * 8)) & 0xFFFF;
}

void write16(GuestMemory& memory, std::uint32_t address, std::uint32_t value) {
  const std::uint32_t word = address & ~3u, shift = (address & 2) * 8;
  const std::uint32_t old = memory.read32(word);
  memory.write32(word, (old & ~(0xFFFFu << shift)) | ((value & 0xFFFF) << shift));
}

bool flag_set(const GuestMemory& memory, std::uint32_t flag) {
  const std::uint32_t byte = StickWalk::kFlags + (flag >> 3);
  const std::uint32_t word = memory.read32(byte & ~3u) >> ((byte & 3) * 8);
  return ((word >> (flag & 7)) & 1) != 0;
}

}  // namespace

StickPush stick_push(int x, int y) {
  StickPush push;
  const double length = std::hypot(static_cast<double>(x), static_cast<double>(y));
  const double reach = std::min(length / 32767.0, 1.0);
  if (reach < StickWalk::kDeadZone || length <= 0) return push;
  push.pushed = true;
  push.x = x / length;
  push.y = y / length;
  push.amount = std::clamp((reach - StickWalk::kDeadZone) / (StickWalk::kFullPush - StickWalk::kDeadZone), 0.0, 1.0);
  return push;
}

int pace_for(const StickPush& push) {
  const double pace = StickWalk::kWalkPace + push.amount * (StickWalk::kRunPace - StickWalk::kWalkPace);
  return std::clamp(static_cast<int>(std::lround(pace)), StickWalk::kWalkPace, StickWalk::kRunPace);
}

FieldStep field_step(const StickPush& push, std::uint16_t camera_angle, std::int32_t length) {
  // As the game turns the d-pad's direction (x right, z down the screen):
  // by a matrix with cos and -sin in its first row, sin and cos in its
  // third, the direction a row on its left.
  const double angle = camera_angle * (2 * kPi / 65536.0);
  const double s = std::sin(angle), c = std::cos(angle);
  const double x = push.x * c + push.y * s, z = -push.x * s + push.y * c;
  return {static_cast<std::int32_t>(std::lround(x * length)), static_cast<std::int32_t>(std::lround(z * length))};
}

bool StickWalk::find_field(const GuestMemory& memory, Field& field) {
  if (memory.read32(kGameCodeAddress) != kGameCode || ((memory.read32(kVersionWord) >> 16) & 0xFF) != 0) return false;
  const std::uint32_t scene = memory.read32(kScenePointer);
  if (!in_main_memory(scene) || memory.read32(scene) != kFieldControlTable) return false;
  if (read16(memory, kState + kModeOffset) == kOtherMode) return false;
  const std::uint32_t number =
      flag_set(memory, kSixthCharacterFlag) ? kSixthCharacter : read16(memory, kState + kCharacterOffset);
  if (number >= kCharacterCount) return false;
  const std::uint32_t character = memory.read32(kCharacters + 4 * number);
  if (!in_main_memory(character)) return false;
  field.control = scene;
  field.character = character;
  return true;
}

void StickWalk::write_pace(GuestMemory& memory, std::uint32_t control, std::int32_t pace) {
  memory.write32(control + kPaceOffset, static_cast<std::uint32_t>(pace));
  control_ = control;
  pace_written_ = pace;
}

void StickWalk::walk(GuestMemory& memory, const Field& field, const StickPush& push, bool run) {
  const std::uint32_t control = field.control;
  const std::uint32_t x = memory.read32(field.character + kPositionOffset);
  const std::uint32_t z = memory.read32(field.character + kPositionOffset + 8);
  const FieldStep step = field_step(push, static_cast<std::uint16_t>(read16(memory, control + kCameraAngleOffset)),
                                    kTargetDistance);
  memory.write32(control + kWalkTargetOffset, x + static_cast<std::uint32_t>(step.x));
  memory.write32(control + kWalkTargetOffset + 8, z + static_cast<std::uint32_t>(step.z));
  write16(memory, control + kWalkFramesOffset, kWalkFrames);
  // The game takes kPaceDecay off before it moves, with no direction held.
  write_pace(memory, control, (run ? kRunPace : pace_for(push)) + kPaceDecay);
  walk_written_ = true;
  walk_control_ = control;
}

void StickWalk::stop(GuestMemory& memory, const Field* field) {
  // Only into the object written before: another may be at the same place.
  if (walk_written_ && field != nullptr && field->control == walk_control_)
    write16(memory, walk_control_ + kWalkFramesOffset, 0);
  walk_written_ = false;
}

std::uint32_t StickWalk::frame(GuestMemory& memory, const Input& input) {
  Field field;
  const bool on_field = input.enabled && find_field(memory, field);
  const Field* here = on_field ? &field : nullptr;
  // Whether the game's control ran since the pace was written: it changes
  // the pace each time it runs.
  const bool control_ran = on_field && control_ == field.control &&
                           static_cast<std::int32_t>(memory.read32(control_ + kPaceOffset)) != pace_written_;
  control_ = 0;
  ++frames_in_mode_;
  const auto go_to = [this](Mode mode) {
    mode_ = mode;
    frames_in_mode_ = 0;
    frames_without_control_ = 0;
  };

  const StickPush push = stick_push(input.stick_x, input.stick_y);
  const bool own_input = (input.held & kDirections) != 0 || input.touching;
  if (!on_field || !push.pushed || own_input) {
    stop(memory, here);
    if (!on_field && push.pushed) {
      if (mode_ != Mode::Pad) go_to(Mode::Pad);
    } else if (mode_ != Mode::Idle) {
      go_to(Mode::Idle);
    }
    // Off the field the stick is the d-pad; on it, the game's own input
    // wins.
    return own_input && on_field ? input.held : input.held | input.stick_directions;
  }

  switch (mode_) {
    case Mode::Idle:
      go_to(Mode::Finding);
      break;
    case Mode::Finding:
      if (control_ran) {
        go_to(Mode::Walking);
      } else if (frames_in_mode_ >= kFirstFrames) {
        stop(memory, here);
        go_to(Mode::Pad);
      }
      break;
    case Mode::Walking:
      frames_without_control_ = control_ran ? 0 : frames_without_control_ + 1;
      if (frames_without_control_ > kLostFrames) {
        stop(memory, here);
        go_to(Mode::Pad);
      }
      break;
    case Mode::Pad:
      if (control_ran) go_to(Mode::Walking);
      break;
  }

  if (mode_ == Mode::Pad) {
    // The stick is the d-pad; the pace shows when the game's control is back.
    write_pace(memory, field.control, kPadPace);
    return input.held | input.stick_directions;
  }
  // B is the run: any button held stops the game's walk to a point.
  walk(memory, field, push, (input.held & kB) != 0);
  return input.held & ~kB;
}

}  // namespace bd::game
