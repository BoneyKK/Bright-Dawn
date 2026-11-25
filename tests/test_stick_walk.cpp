// The left stick's walk on the field (game/stick_walk.h): the push, the
// pace, the direction on the field, and the choice between the stick, the
// d-pad and the stylus, on a main memory laid out as the game has it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "game/stick_walk.h"
#include "input/bindings.h"
#include "test.h"

namespace {

using bd::game::StickWalk;

class Memory : public bd::game::GuestMemory {
 public:
  std::uint32_t read32(std::uint32_t address) const override {
    const auto i = words.find(address);
    return i == words.end() ? 0 : i->second;
  }
  void write32(std::uint32_t address, std::uint32_t value) override { words[address] = value; }
  std::unordered_map<std::uint32_t, std::uint32_t> words;
};

constexpr std::uint32_t kControl = 0x02260788, kCharacter = 0x022f4ac4;
constexpr std::uint32_t kRight = 1u << 4, kUp = 1u << 6;
constexpr std::int32_t kX = 0x10000, kZ = -0x8000;

// The field, as the game has it: the release, the scene's control object
// and the character 0 at (kX, kZ).
Memory field() {
  Memory m;
  m.words[StickWalk::kGameCodeAddress] = StickWalk::kGameCode;
  m.words[StickWalk::kScenePointer] = kControl;
  m.words[kControl] = StickWalk::kFieldControlTable;
  m.words[StickWalk::kCharacters] = kCharacter;
  m.words[kCharacter + StickWalk::kPositionOffset] = static_cast<std::uint32_t>(kX);
  m.words[kCharacter + StickWalk::kPositionOffset + 8] = static_cast<std::uint32_t>(kZ);
  return m;
}

std::int32_t word(const Memory& m, std::uint32_t address) { return static_cast<std::int32_t>(m.read32(address)); }
std::uint32_t walk_frames(const Memory& m) { return m.read32(kControl + StickWalk::kWalkFramesOffset) & 0xffff; }
std::int32_t pace(const Memory& m) { return word(m, kControl + StickWalk::kPaceOffset); }

// The game's control runs a frame with no direction held: the pace down.
void control_runs(Memory& m) {
  m.words[kControl + StickWalk::kPaceOffset] = static_cast<std::uint32_t>(pace(m) - StickWalk::kPaceDecay);
}

// The stick at (`x`, `y`) percent, as the program gives it.
StickWalk::Input stick(int x, int y, std::uint32_t held = 0) {
  StickWalk::Input input;
  input.held = held;
  input.stick_x = x * 32767 / 100;
  input.stick_y = y * 32767 / 100;
  input.stick_directions = bd::input::directions_from_stick(input.stick_x, input.stick_y);
  return input;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

}  // namespace

TEST(stick_push_has_a_dead_zone_and_eases_to_full) {
  using bd::game::stick_push;
  CHECK(!stick_push(0, 0).pushed);
  CHECK(!stick_push(6500, 0).pushed);  // 19.8%: in the dead zone.
  const auto edge = stick_push(6600, 0);
  CHECK(edge.pushed && edge.amount < 0.01 && near(edge.x, 1) && near(edge.y, 0));
  // Half way between the dead zone (20%) and full (90%).
  CHECK(near(stick_push(0, -static_cast<int>(0.55 * 32767)).amount, 0.5));
  CHECK(near(stick_push(static_cast<int>(0.9 * 32767) + 1, 0).amount, 1));
  CHECK(near(stick_push(-32768, 0).amount, 1));
}

TEST(stick_push_is_the_same_in_every_direction) {
  using bd::game::stick_push;
  for (int degrees = 0; degrees < 360; degrees += 15) {
    const double a = degrees * 3.14159265358979 / 180;
    for (const double reach : {0.1, 0.4, 0.7, 1.0}) {
      const auto push = stick_push(static_cast<int>(std::lround(reach * 32767 * std::cos(a))),
                                   static_cast<int>(std::lround(reach * 32767 * std::sin(a))));
      CHECK(push.pushed == (reach >= StickWalk::kDeadZone));
      if (!push.pushed) continue;
      CHECK(std::fabs(push.amount - std::min((reach - 0.2) / 0.7, 1.0)) < 1e-3);
      CHECK(near(push.x, std::cos(a)) && near(push.y, std::sin(a)));
    }
  }
  // A corner of a square stick's reach is full, not more.
  CHECK(near(stick_push(32767, 32767).amount, 1));
}

TEST(stick_pace_goes_from_the_walk_to_the_run) {
  using bd::game::pace_for;
  using bd::game::stick_push;
  CHECK(pace_for(stick_push(6600, 0)) == StickWalk::kWalkPace);
  CHECK(pace_for(stick_push(static_cast<int>(0.55 * 32767), 0)) == 13);
  CHECK(pace_for(stick_push(32767, 0)) == StickWalk::kRunPace);
  int last = 0;
  for (int x = 6600; x <= 32767; x += 500) {
    const int p = pace_for(stick_push(x, 0));
    CHECK(p >= last);
    last = p;
  }
}

TEST(stick_step_is_turned_by_the_camera_as_the_d_pad_is) {
  using bd::game::field_step;
  using bd::game::stick_push;
  const auto right = stick_push(32767, 0), down = stick_push(0, 32767);
  // Camera straight: right is +x, down the screen is +z.
  CHECK(field_step(right, 0, 0x4000).x == 0x4000 && field_step(right, 0, 0x4000).z == 0);
  CHECK(field_step(down, 0, 0x4000).x == 0 && field_step(down, 0, 0x4000).z == 0x4000);
  // A quarter turn: x' = x cos + z sin, z' = -x sin + z cos.
  CHECK(field_step(right, 0x4000, 0x4000).x == 0 && field_step(right, 0x4000, 0x4000).z == -0x4000);
  CHECK(field_step(down, 0x4000, 0x4000).x == 0x4000 && field_step(down, 0x4000, 0x4000).z == 0);
  // Any angle: the same length.
  const auto diagonal = stick_push(-20000, 9000);
  for (std::uint32_t angle = 0; angle < 0x10000; angle += 0x1234) {
    const auto step = field_step(diagonal, static_cast<std::uint16_t>(angle), 0x4000);
    CHECK(std::fabs(std::hypot(step.x, step.z) - 0x4000) < 2);
  }
}

TEST(stick_walks_where_the_game_has_control) {
  Memory m = field();
  StickWalk walk;
  // Half pushed, right: no d-pad for the game; a walk to a point to the
  // right of the character, at the pace for the push plus what the game
  // takes off.
  CHECK(walk.frame(m, stick(55, 0)) == 0u);
  CHECK(walk.mode() == StickWalk::Mode::Finding);
  CHECK(walk_frames(m) == StickWalk::kWalkFrames);
  CHECK(word(m, kControl + StickWalk::kWalkTargetOffset) == kX + StickWalk::kTargetDistance);
  CHECK(word(m, kControl + StickWalk::kWalkTargetOffset + 8) == kZ);
  CHECK(pace(m) == 13 + StickWalk::kPaceDecay);
  // The game's control runs: the stick has the field.
  control_runs(m);
  CHECK(walk.frame(m, stick(0, -100)) == 0u);
  CHECK(walk.mode() == StickWalk::Mode::Walking);
  CHECK(word(m, kControl + StickWalk::kWalkTargetOffset + 8) == kZ - StickWalk::kTargetDistance);
  CHECK(pace(m) == StickWalk::kRunPace + StickWalk::kPaceDecay);
  // B runs, and is not given to the game: a button held stops its walk.
  control_runs(m);
  CHECK(walk.frame(m, stick(30, 0, StickWalk::kB | 1u)) == 1u);
  CHECK(pace(m) == StickWalk::kRunPace + StickWalk::kPaceDecay);
  // Let go: the walk ends at once.
  control_runs(m);
  CHECK(walk.frame(m, stick(5, 0)) == 0u);
  CHECK(walk.mode() == StickWalk::Mode::Idle && walk_frames(m) == 0u);
}

TEST(stick_gives_way_to_the_d_pad_and_the_stylus) {
  Memory m = field();
  StickWalk walk;
  walk.frame(m, stick(100, 0));
  control_runs(m);
  walk.frame(m, stick(100, 0));
  CHECK(walk.mode() == StickWalk::Mode::Walking);
  // The d-pad held: the game has it, and the stick's walk ends.
  CHECK(walk.frame(m, stick(100, 0, kUp)) == kUp);
  CHECK(walk.mode() == StickWalk::Mode::Idle && walk_frames(m) == 0u);
  // The stylus on the screen: the same, with no d-pad from the stick.
  walk.frame(m, stick(100, 0));
  StickWalk::Input touch = stick(100, 0);
  touch.touching = true;
  CHECK(walk.frame(m, touch) == 0u);
  CHECK(walk.mode() == StickWalk::Mode::Idle && walk_frames(m) == 0u);
}

TEST(stick_is_the_d_pad_where_the_game_has_no_control) {
  // A text box or a menu on the field: the game's control does not run.
  Memory m = field();
  StickWalk walk;
  for (int i = 0; i < StickWalk::kFirstFrames; ++i) CHECK(walk.frame(m, stick(100, 0)) == 0u);
  CHECK(walk.frame(m, stick(100, 0)) == kRight);
  CHECK(walk.mode() == StickWalk::Mode::Pad && walk_frames(m) == 0u);
  CHECK(pace(m) == StickWalk::kPadPace);
  CHECK(walk.frame(m, stick(100, 0)) == kRight);
  // The control is back: the stick walks again.
  control_runs(m);
  CHECK(walk.frame(m, stick(100, 0)) == 0u);
  CHECK(walk.mode() == StickWalk::Mode::Walking);
  // While walking, a text box opens: after kLostFrames the stick is the
  // d-pad again.
  int frames = 0;
  while (walk.mode() == StickWalk::Mode::Walking && frames < 100) {
    walk.frame(m, stick(100, 0));
    ++frames;
  }
  CHECK(frames == StickWalk::kLostFrames + 1 && walk.mode() == StickWalk::Mode::Pad && walk_frames(m) == 0u);
}

TEST(stick_is_the_d_pad_off_the_field_and_in_other_releases) {
  StickWalk walk;
  // A battle or a menu of its own: another scene object.
  Memory battle = field();
  battle.words[kControl] = 0x021d0000;
  CHECK(walk.frame(battle, stick(100, 0)) == kRight);
  CHECK(walk.mode() == StickWalk::Mode::Pad);
  CHECK(battle.read32(kControl + StickWalk::kWalkFramesOffset) == 0u && pace(battle) == 0);
  // Another release, another revision, or the setting off.
  Memory other = field();
  other.words[StickWalk::kGameCodeAddress] = 0x45354f42;  // "BO5E".
  CHECK(StickWalk().frame(other, stick(100, 0)) == kRight && pace(other) == 0);
  Memory revision = field();
  revision.words[StickWalk::kVersionWord] = 1u << 16;
  CHECK(StickWalk().frame(revision, stick(100, 0)) == kRight && pace(revision) == 0);
  Memory off = field();
  StickWalk::Input input = stick(100, 0);
  input.enabled = false;
  CHECK(StickWalk().frame(off, input) == kRight && pace(off) == 0);
  // Another control of the character (mode 4), or no character.
  Memory mode = field();
  mode.words[(StickWalk::kState + StickWalk::kModeOffset) & ~3u] = StickWalk::kOtherMode << 16;
  CHECK(StickWalk().frame(mode, stick(100, 0)) == kRight && pace(mode) == 0);
  Memory nobody = field();
  nobody.words[StickWalk::kCharacters] = 0;
  CHECK(StickWalk().frame(nobody, stick(100, 0)) == kRight && pace(nobody) == 0);
}

TEST(stick_walk_ends_only_in_the_object_it_was_written_to) {
  Memory m = field();
  StickWalk walk;
  walk.frame(m, stick(100, 0));
  // A new field: another control object, whose own walk is kept.
  m.words[StickWalk::kScenePointer] = kControl + 0x2000;
  m.words[kControl + 0x2000] = StickWalk::kFieldControlTable;
  m.words[kControl + 0x2000 + StickWalk::kWalkFramesOffset] = 7;
  walk.frame(m, stick(0, 0));
  CHECK(m.read32(kControl + 0x2000 + StickWalk::kWalkFramesOffset) == 7u);
}

TEST(scripted_sticks_are_read_and_found_by_frame) {
  using namespace bd::input;
  const auto push = parse_scripted_stick("70,-70@1000-1100");
  REQUIRE(push.has_value());
  CHECK(push->x == 70 && push->y == -70 && push->first == 1000 && push->last == 1100);
  const auto one = parse_scripted_stick("-100,0@5");
  REQUIRE(one.has_value());
  CHECK(one->x == -100 && one->y == 0 && one->first == 5 && one->last == 5);
  for (const char* bad : {"70,-70", "70@5", "101,0@5", "0,-101@5", "--1,0@5", "1,,2@5", "1,2@0", "a,2@5", ",2@5"})
    CHECK(!parse_scripted_stick(bad).has_value());
  const std::vector<ScriptedStick> script = {*push, *one};
  CHECK(scripted_stick(script, 999) == nullptr);
  CHECK(scripted_stick(script, 1000) == &script[0]);
  CHECK(scripted_stick(script, 5) == &script[1]);
}
