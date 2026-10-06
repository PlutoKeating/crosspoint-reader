#pragma once
#include <I18n.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Settings: one list of system screens (Wi-Fi, Bluetooth, firmware update,
// SD-card firmware update). The display and controls tabs were removed in
// 2.7.4 and the language picker in 2.7.5 (the firmware is Chinese-only).
// Back returns to the StockStick page, the device's home.
class SettingsActivity final : public Activity {
  enum class Item : uint8_t { Network, Bluetooth, FirmwareUpdate, SdFirmwareUpdate };
  static constexpr int ITEM_COUNT = 4;
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
