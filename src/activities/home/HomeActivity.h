#pragma once
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// System menu reached with Back from the StockStick surface. The firmware no
// longer ships an ebook library, so Home only routes to product surfaces.
class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  // Home is usually entered while Back is still held; ignore that stale
  // release until a fresh press is seen here.
  bool backPressSeen = false;
  const HomeMenuItem initialMenuItem;

  static constexpr int MENU_ITEM_COUNT = 2;  // StockStick, Settings
  static HomeMenuItem indexToMenuItem(int idx) {
    return idx == 0 ? HomeMenuItem::PROJECT_STICK : HomeMenuItem::SETTINGS_MENU;
  }
  static int menuItemToIndex(HomeMenuItem item) { return item == HomeMenuItem::SETTINGS_MENU ? 1 : 0; }
  void activateSelection();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
