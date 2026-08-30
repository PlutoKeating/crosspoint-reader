#pragma once

#include <cstdint>

namespace project_stick {

class Keyguard {
 public:
  enum class State : uint8_t { Unlocked, AwaitLeft, AwaitRight };
  enum class Input : uint8_t { None, Activity, LeftSide, RightSide, OtherButton };

  static constexpr uint32_t LOCK_AFTER_MS = 20UL * 1000UL;

  void begin(uint32_t nowMs) {
    state_ = State::Unlocked;
    lastActivityMs_ = nowMs;
  }

  bool update(uint32_t nowMs, Input input = Input::None) {
    const State previous = state_;
    if (state_ == State::Unlocked) {
      if (input != Input::None) {
        lastActivityMs_ = nowMs;
      } else if (nowMs - lastActivityMs_ >= LOCK_AFTER_MS) {
        state_ = State::AwaitLeft;
      }
      return state_ != previous;
    }

    if (input == Input::LeftSide) {
      state_ = State::AwaitRight;
    } else if (input == Input::RightSide) {
      if (state_ == State::AwaitRight) {
        state_ = State::Unlocked;
        lastActivityMs_ = nowMs;
      } else {
        state_ = State::AwaitLeft;
      }
    } else if (input == Input::OtherButton) {
      state_ = State::AwaitLeft;
    }
    return state_ != previous;
  }

  [[nodiscard]] State state() const { return state_; }
  [[nodiscard]] bool locked() const { return state_ != State::Unlocked; }

 private:
  State state_ = State::Unlocked;
  uint32_t lastActivityMs_ = 0;
};

}  // namespace project_stick
