#pragma once
#include "activities/Activity.h"

// Power-key sleep: the card (or whatever page was open) stays on screen and a
// small moon in the corner marks the device as asleep. Custom sleep images and
// the dark/light/blank sleep screens were removed in 2.7.4.
class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;
  StickTakeover stickTakeover() const override { return StickTakeover::Never; }
};
