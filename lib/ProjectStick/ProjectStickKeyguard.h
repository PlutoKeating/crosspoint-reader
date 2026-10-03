#pragma once

#include <cstdint>

namespace project_stick {

class Keyguard {
 public:
  enum class State : uint8_t { Unlocked, AwaitLeft, AwaitRight };
  enum class Input : uint8_t { None, Activity, ButtonActivity, LeftSide, RightSide, OtherButton };

  static constexpr uint32_t LOCK_AFTER_MS = 20UL * 1000UL;
  // The unlock guide hides again after this long without a key action while
  // still locked (same timeout as the button hints); a half-done unlock
  // (left step taken) is forgotten with it.
  static constexpr uint32_t PROMPT_TIMEOUT_MS = 5UL * 1000UL;
  // After the left step the unlock cue slides toward the right side key in a
  // few discrete, e-paper-safe frames.
  static constexpr uint32_t CUE_FRAME_MS = 150;
  static constexpr uint8_t CUE_FINAL_FRAME = 3;

  void begin(uint32_t nowMs) {
    state_ = State::Unlocked;
    lastActivityMs_ = nowMs;
    promptVisible_ = false;
  }

  bool update(uint32_t nowMs, Input input = Input::None) {
    const State previous = state_;
    const bool previousPromptVisible = promptVisible_;
    if (state_ == State::Unlocked) {
      if (input != Input::None) {
        lastActivityMs_ = nowMs;
      } else if (nowMs - lastActivityMs_ >= LOCK_AFTER_MS) {
        state_ = State::AwaitLeft;
        promptVisible_ = false;
      }
      return state_ != previous || promptVisible_ != previousPromptVisible;
    }

    if (input != Input::None) {
      promptVisible_ = true;
      promptSinceMs_ = nowMs;
    } else if (promptVisible_ && nowMs - promptSinceMs_ >= PROMPT_TIMEOUT_MS) {
      promptVisible_ = false;
      state_ = State::AwaitLeft;
      return state_ != previous || promptVisible_ != previousPromptVisible;
    }
    if (input == Input::LeftSide) {
      if (state_ != State::AwaitRight) awaitRightSinceMs_ = nowMs;
      state_ = State::AwaitRight;
    } else if (input == Input::RightSide) {
      if (state_ == State::AwaitRight) {
        state_ = State::Unlocked;
        lastActivityMs_ = nowMs;
        promptVisible_ = false;
      } else {
        state_ = State::AwaitLeft;
      }
    } else if (input == Input::OtherButton) {
      state_ = State::AwaitLeft;
    }
    return state_ != previous || promptVisible_ != previousPromptVisible;
  }

  [[nodiscard]] State state() const { return state_; }
  [[nodiscard]] bool locked() const { return state_ != State::Unlocked; }
  [[nodiscard]] bool promptVisible() const { return promptVisible_; }
  // Frame of the "now press the right key" cue; 0 outside AwaitRight.
  [[nodiscard]] uint8_t cueFrame(uint32_t nowMs) const {
    if (state_ != State::AwaitRight) return 0;
    const uint32_t frame = (nowMs - awaitRightSinceMs_) / CUE_FRAME_MS;
    return static_cast<uint8_t>(frame < CUE_FINAL_FRAME ? frame : CUE_FINAL_FRAME);
  }

 private:
  State state_ = State::Unlocked;
  uint32_t lastActivityMs_ = 0;
  uint32_t awaitRightSinceMs_ = 0;
  uint32_t promptSinceMs_ = 0;
  bool promptVisible_ = false;
};

}  // namespace project_stick
