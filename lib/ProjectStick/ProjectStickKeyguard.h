#pragma once

#include <cstdint>

namespace project_stick {

class Keyguard {
 public:
  enum class State : uint8_t { Unlocked, AwaitLeft, AwaitRight };
  enum class Input : uint8_t { None, Activity, ButtonActivity, LeftSide, RightSide, OtherButton };

  static constexpr uint32_t LOCK_AFTER_MS = 20UL * 1000UL;

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

    if (input != Input::None) promptVisible_ = true;
    if (input == Input::LeftSide) {
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

 private:
  State state_ = State::Unlocked;
  uint32_t lastActivityMs_ = 0;
  bool promptVisible_ = false;
};

}  // namespace project_stick
