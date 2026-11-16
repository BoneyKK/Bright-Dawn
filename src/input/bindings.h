// Which keys and controller buttons press which of the DS's buttons.
//
// Controls are named as SDL names them (SDL_GetScancodeName for keys,
// SDL_GetGamepadStringForButton for controller buttons), so that bindings can
// be written down and read back without this module knowing about SDL. The
// window resolves the names once, when it starts.
//
// Controller buttons are bound by position, as on the DS: its A is the right
// face button, B the bottom one, X the top one and Y the left one. SDL names
// the bottom face button "a", the right one "b", the left one "x" and the top
// one "y", whatever is printed on them.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd::input {

// The DS's buttons, in the order of the bits of nds::Keypad::set_held.
enum class DsButton : unsigned { A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y };
inline constexpr unsigned kDsButtonCount = 12;

constexpr std::uint32_t bit(DsButton button) { return 1u << static_cast<unsigned>(button); }

// The DS button's name: "A", "B", "Select", "Start", "Right", "Left", "Up",
// "Down", "R", "L", "X", "Y".
const char* name(DsButton button);
// The DS button of that name, in any letter case, or nothing.
std::optional<DsButton> button_named(std::string_view name);

struct Binding {
  std::string control;  // As SDL names it.
  DsButton button;
};

// The bindings used when none are set: every DS button has a key and a
// controller button, and no control presses two DS buttons.
const std::vector<Binding>& default_keyboard_bindings();
const std::vector<Binding>& default_controller_bindings();

// How far a controller's left stick must be pushed (of 32767) to press the
// direction it points in, and how far a trigger must be pulled to press L or R.
inline constexpr int kStickThreshold = 16384;
inline constexpr int kTriggerThreshold = 16384;

// A button held over a range of frames, from a script rather than a person:
// for reproducing what was done, and for checking the game's response
// without a keyboard. Written "Start@300-310" (frames 300 to 310, counting
// from 1) or "A@42" (one frame).
struct ScriptedPress {
  DsButton button;
  std::uint64_t first = 0, last = 0;
};
std::optional<ScriptedPress> parse_scripted_press(std::string_view text);
// The buttons the script holds in `frame`.
std::uint32_t scripted_buttons(const std::vector<ScriptedPress>& script, std::uint64_t frame);

// A touch of the touch screen from a script, at a pixel (0-255 across, 0-191
// down) over a range of frames. Written "128,96@600-610" or "10,20@42".
struct ScriptedTouch {
  int x = 0, y = 0;
  std::uint64_t first = 0, last = 0;
};
std::optional<ScriptedTouch> parse_scripted_touch(std::string_view text);
// The script's touch in `frame`, if any: the first that covers it.
const ScriptedTouch* scripted_touch(const std::vector<ScriptedTouch>& script, std::uint64_t frame);

// The DS directions pressed by a stick at (`x`, `y`), each -32768 to 32767,
// down positive: those past the threshold on each axis.
std::uint32_t directions_from_stick(int x, int y);

}  // namespace bd::input
