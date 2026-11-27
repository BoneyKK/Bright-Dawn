// Frame by frame (launcher, Debug page: Pause and step; --pause-at,
// --step-count): when the emulated machine may run its next frame.
//
// The game screen asks after each frame it has shown (after_frame()); while
// paused() it holds the machine (no game code, timing, sound or input) and
// only redraws the window, until a step or resume. A step runs exactly one
// frame, then holds again.
//
// Scripted (--pause-at N, --step-count M): the machine holds after frame N;
// then it takes M steps by itself, one each time the window is redrawn
// (held_redrawn()), and runs on after the last. Without --step-count it
// holds until a hotkey.
#pragma once

#include <cstdint>

namespace bd::frame_log {

class FrameStepper {
 public:
  // Hotkeys. pause(): hold after the frame being run. step(): while held,
  // run one frame. resume(): run on.
  void pause() { pause_asked_ = true; }
  void step() {
    if (!paused_) return;
    paused_ = false;
    stepping_ = true;
  }
  void resume() {
    paused_ = stepping_ = pause_asked_ = run_on_ = false;
    steps_left_ = 0;
  }
  // Scripted: hold after frame `at` (0: never), then take `steps` steps.
  void script(std::uint64_t at, std::uint64_t steps) {
    pause_at_ = at;
    steps_after_ = steps;
  }

  // After frame `frame` (1 for the first) was run and shown: true when the
  // machine is to hold before the next.
  bool after_frame(std::uint64_t frame) {
    if (stepping_) {
      stepping_ = false;
      paused_ = !run_on_;
      run_on_ = false;
    }
    if (pause_at_ != 0 && frame == pause_at_) {
      paused_ = true;
      steps_left_ = steps_after_;
    }
    if (pause_asked_) {
      pause_asked_ = false;
      paused_ = true;
    }
    return paused_;
  }
  // While held, each time the window was redrawn: takes a scripted step
  // (after the last, the machine runs on). True when the machine may run
  // its next frame.
  bool held_redrawn() {
    if (!paused_) return true;
    if (steps_left_ == 0) return false;
    --steps_left_;
    step();
    run_on_ = steps_left_ == 0;
    return true;
  }

  bool paused() const { return paused_; }

 private:
  bool paused_ = false, stepping_ = false, pause_asked_ = false, run_on_ = false;
  std::uint64_t pause_at_ = 0, steps_after_ = 0, steps_left_ = 0;
};

}  // namespace bd::frame_log
