// Walking with a controller's left stick on the field of Golden Sun: Dark
// Dawn, at any angle and at any pace from the game's walk to its run.
//
// How the game moves the character: while the player has control on the
// field, the field's control object (its virtual table kFieldControlTable;
// the current scene object at kScenePointer) runs a function each game
// frame that reads the buttons and the touch screen:
// - the d-pad gives one of eight directions, turned by the camera's angle;
//   its pace (kPaceOffset, 0 to kRunPace) goes up by one each game frame a
//   direction is held, down by kPaceDecay when none is, and to kRunPace
//   while B is held; the character moves at the larger of
//   pace * 384 / 20 + 128 and 256 (1/4096ths of a unit a frame);
// - a held touch walks toward the touched point, at a pace by its distance
//   from the character on the screen;
// - with no button held and no touch, it walks toward a point of its own
//   (kWalkTargetOffset: x, then z 8 bytes on) while a count of game frames
//   (kWalkFramesOffset) is above 0, one less each frame, at the pace above.
//
// The stick uses the last: each frame it is pushed past the dead zone, the
// point is set kTargetDistance from the character (kPositionOffset) in the
// stick's direction turned by the camera's angle (kCameraAngleOffset), the
// count to kWalkFrames, and the pace to the stick's, plus the kPaceDecay the
// game takes off. A direction or B held, or a touch, is the game's own
// input; the stick then does nothing. The touch was not used: it needs the
// character's place on the screen, which the game works out each frame and
// keeps only while it does.
//
// Field control: only the game's control function changes the pace. Each
// frame the stick is used on the field, the pace written is compared with
// the pace found the next frame; a change means the function ran, so the
// player has control. Where it does not run (a text box, a menu, an event)
// or there is no field (battles, menus of their own), the stick is the
// d-pad, as without this.
//
// The addresses are those of the European release (game code BO5P, ROM
// version 0); with any other, the stick is the d-pad.
#pragma once

#include <cstdint>

#include "game/opening_skip.h"

namespace bd::game {

// A stick's push, cut by the dead zone and eased to full: a direction (a
// unit vector, x right and y down) and how far, 0 (inside the dead zone) to
// 1 (at kFullPush and beyond). The same in every direction.
struct StickPush {
  bool pushed = false;
  double x = 0, y = 0;
  double amount = 0;
};

// The push of a stick at (`x`, `y`), each -32768 to 32767.
StickPush stick_push(int x, int y);

// The game's pace (kWalkPace to kRunPace) for a push.
int pace_for(const StickPush& push);

// Where a push points on the field: the direction on the screen turned by
// the camera's angle (`camera_angle`, 65536 a turn), as the game turns the
// d-pad's. In 1/4096ths of a unit: `x` and `z` of `length` together.
struct FieldStep {
  std::int32_t x = 0, z = 0;
};
FieldStep field_step(const StickPush& push, std::uint16_t camera_angle, std::int32_t length);

class StickWalk {
 public:
  // The buttons, as bits of the DS's button register.
  static constexpr std::uint32_t kB = 1u << 1;
  static constexpr std::uint32_t kDirections = 0xF0;  // Right, Left, Up, Down.

  // The dead zone and the push for the run, of the stick's full reach.
  static constexpr double kDeadZone = 0.2, kFullPush = 0.9;

  // The release: the cartridge header's copy in main memory, its game code
  // and version.
  static constexpr std::uint32_t kGameCodeAddress = 0x027FFE0C, kGameCode = 0x50354F42;  // "BO5P".
  static constexpr std::uint32_t kVersionWord = 0x027FFE1C;  // The version is its third byte.

  // The current scene object, and the virtual table of the field's control
  // object.
  static constexpr std::uint32_t kScenePointer = 0x020792FC, kFieldControlTable = 0x021DCFAC;
  // In the field's control object: the count of frames and the point to
  // walk to (16-bit; x, z 8 bytes on, 32-bit), the pace (32-bit) and the
  // camera's angle (16-bit).
  static constexpr std::uint32_t kWalkFramesOffset = 0x4CC, kWalkTargetOffset = 0x4D0;
  static constexpr std::uint32_t kPaceOffset = 0x12C4, kCameraAngleOffset = 0x284;
  static constexpr int kWalkPace = 6, kRunPace = 20, kPaceDecay = 4;

  // The character the player moves: the game's state (kState) holds the
  // control mode (16-bit, at kModeOffset; kOtherMode is a control of another
  // kind) and the character's number (16-bit, at kCharacterOffset), unless
  // flag kSixthCharacterFlag (of the flags at kFlags) is set, which makes it
  // kSixthCharacter. Characters 0 to 7 are pointed to from kCharacters; a
  // character's place (x, y, z, 32-bit) is at kPositionOffset.
  static constexpr std::uint32_t kState = 0x0207B204, kModeOffset = 0x2C42, kCharacterOffset = 0x2BEC;
  static constexpr std::uint32_t kOtherMode = 4;
  static constexpr std::uint32_t kFlags = 0x0207BD0C, kSixthCharacterFlag = 47, kSixthCharacter = 5;
  static constexpr std::uint32_t kCharacters = 0x021ED664, kCharacterCount = 8;
  static constexpr std::uint32_t kPositionOffset = 0x5C;

  // How far ahead the point is put (4 units; the game stops at half of one),
  // and the count of frames written: the walk goes on for these game frames
  // when the program stops writing it.
  static constexpr std::int32_t kTargetDistance = 0x4000;
  static constexpr std::uint32_t kWalkFrames = 4;
  // The pace written while the stick is the d-pad on the field, to see the
  // game's control come back; and the frames with no sign of it before the
  // stick is the d-pad.
  static constexpr std::int32_t kPadPace = 10;
  static constexpr int kFirstFrames = 4, kLostFrames = 12;

  enum class Mode {
    Idle,     // The stick is not pushed, or not used.
    Finding,  // Pushed: walking, until the game shows (or does not) that it has control.
    Walking,  // The game has control: the stick walks.
    Pad,      // No control or no field: the stick is the d-pad.
  };

  struct Input {
    std::uint32_t held = 0;               // The buttons from all else (keys, controllers, a script).
    int stick_x = 0, stick_y = 0;         // The left stick, -32768 to 32767.
    std::uint32_t stick_directions = 0;   // The d-pad as the stick presses it.
    bool touching = false;                // The stylus.
    bool enabled = true;                  // Settings, Controls.
  };

  // Once a frame, before the game's next: returns the buttons the game is
  // to be given.
  std::uint32_t frame(GuestMemory& memory, const Input& input);

  Mode mode() const { return mode_; }

 private:
  struct Field {
    std::uint32_t control = 0;  // The field's control object.
    std::uint32_t character = 0;
  };
  // The field's control object and the character, or nothing where they are
  // not (or the release is another).
  static bool find_field(const GuestMemory& memory, Field& field);
  void walk(GuestMemory& memory, const Field& field, const StickPush& push, bool run);
  // Ends the walk written, in `field` if it is the same object.
  void stop(GuestMemory& memory, const Field* field);
  void write_pace(GuestMemory& memory, std::uint32_t control, std::int32_t pace);

  Mode mode_ = Mode::Idle;
  int frames_in_mode_ = 0, frames_without_control_ = 0;
  std::uint32_t control_ = 0;   // Where the pace was written, 0 for nowhere.
  std::int32_t pace_written_ = 0;
  bool walk_written_ = false;
  std::uint32_t walk_control_ = 0;  // Where the walk was written.
};

}  // namespace bd::game
