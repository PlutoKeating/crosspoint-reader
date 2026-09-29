#include "HomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <climits>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void HomeActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = menuItemToIndex(initialMenuItem);
  requestUpdate();
}

void HomeActivity::activateSelection() {
  if (indexToMenuItem(selectorIndex) == HomeMenuItem::PROJECT_STICK) {
    activityManager.goToProjectStick();
  } else {
    activityManager.goToSettings();
  }
}

void HomeActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  buttonNavigator.onNext([this] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, MENU_ITEM_COUNT);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = swipe == MappedInputManager::SwipeDir::Up
                        ? ButtonNavigator::nextIndex(selectorIndex, MENU_ITEM_COUNT)
                        : ButtonNavigator::previousIndex(selectorIndex, MENU_ITEM_COUNT);
    requestUpdate();
    return;
  }

  // Back from the system menu always returns to the product surface.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;
  if (backPressSeen && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToProjectStick();
    return;
  }

  const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int menuRow = -1;
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing,
                                              MENU_ITEM_COUNT, 0, INT32_MAX, metrics.menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    selectorIndex = menuRow;
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      requestUpdate();
    } else {
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelection();
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PROJECT_STICK));

  static const StrId labels[MENU_ITEM_COUNT] = {StrId::STR_PROJECT_STICK, StrId::STR_SETTINGS_TITLE};
  static const UIIcon icons[MENU_ITEM_COUNT] = {Wifi, Settings};
  const int menuTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawButtonMenu(
      renderer, Rect{0, menuTop, pageWidth, pageHeight - menuTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
      MENU_ITEM_COUNT, selectorIndex, [](int index) { return std::string(I18n::getInstance().get(labels[index])); },
      [](int index) { return icons[index]; });

  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
