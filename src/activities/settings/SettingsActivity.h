#pragma once
#include <I18n.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Settings: one list of system screens (Wi-Fi, Bluetooth, firmware update,
// SD-card firmware update, language). The display and controls tabs were
// removed in 2.7.4; their behaviour is fixed (see CrossPointSettings.h).
class SettingsActivity final : public Activity {
  enum class Item : uint8_t { Network, Bluetooth, FirmwareUpdate, SdFirmwareUpdate, Language };
  static constexpr int ITEM_COUNT = 5;
  static const Item items[ITEM_COUNT];
  static StrId itemName(Item item);

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void openSelected();

 public:
  explicit SettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Settings", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
