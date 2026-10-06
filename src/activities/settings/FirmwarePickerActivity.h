#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Lists firmware images (*.bin) in the SD root and /firmware so the SD update
// and recovery flows can pick one. Returns FilePathResult, or a cancelled
// result on Back. The ebook file browser this replaces no longer exists.
class FirmwarePickerActivity final : public Activity {
 public:
  explicit FirmwarePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FirmwarePicker", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  StickTakeover stickTakeover() const override { return StickTakeover::Never; }

 private:
  static constexpr size_t MAX_ENTRIES = 32;
  void scanDirectory(const char* directory);
  void choose();

  ButtonNavigator buttonNavigator;
  std::vector<std::string> paths;
  int selectedIndex = 0;
};
