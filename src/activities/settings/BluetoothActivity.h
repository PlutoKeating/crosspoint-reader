#pragma once

#include "activities/Activity.h"

// Settings > System > Bluetooth: the BLE radio's live state (the only content
// channel), what the last start failed on, and the two controls a user needs
// when the mini program cannot find the device: switch it off/on and restart
// the stack.
class BluetoothActivity final : public Activity {
 public:
  explicit BluetoothActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bluetooth", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The radio is being watched: keep the device awake and the loop responsive.
  bool preventAutoSleep() override { return true; }

 private:
  uint32_t renderedGeneration = 0;
  bool backPressSeen = false;
};
