#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

void LanguageSelectActivity::onEnter() {
  Activity::onEnter();

  languages = language_packs::available();
  selectedIndex = 0;
  for (int i = 0; i < itemCount(); ++i) {
    if (languages[i].code == I18N.languageCode()) selectedIndex = i;
  }

  requestUpdate();
}

void LanguageSelectActivity::onExit() {
  Activity::onExit();
  languages.clear();
  languages.shrink_to_fit();
}

void LanguageSelectActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  switch (handleListTouch(selectedIndex, itemCount(), contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      activateSelected();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), itemCount(), pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), itemCount(), pageItems);
    requestUpdate();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), itemCount());
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), itemCount());
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), itemCount(), pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), itemCount(), pageItems);
    requestUpdate();
  });
}

void LanguageSelectActivity::handleSelection() {
  if (selectedIndex < 0 || selectedIndex >= itemCount()) return;
  const std::string code = languages[selectedIndex].code;
  bool applied = false;
  {
    RenderLock lock(*this);
    applied = language_packs::apply(code.c_str());
  }
  if (applied && strcmp(SETTINGS.language, code.c_str()) != 0) {
    strncpy(SETTINGS.language, code.c_str(), sizeof(SETTINGS.language) - 1);
    SETTINGS.language[sizeof(SETTINGS.language) - 1] = '\0';
    SETTINGS.saveToFile();
  }
  onBack();
}

void LanguageSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_LANGUAGE));

  // Current language marker
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const std::string current = I18N.languageCode();
  const int hintHeight = renderer.getLineHeight(SMALL_FONT_ID) * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight - hintHeight}, itemCount(), selectedIndex,
      [this](int index) { return languages[index].name; }, nullptr, nullptr,
      [this, &current](int index) { return languages[index].code == current ? tr(STR_SELECTED) : ""; }, true);
  GUI.drawHelpText(renderer, Rect{0, contentTop + contentHeight - hintHeight, pageWidth, hintHeight},
                   tr(STR_LANGUAGE_PACK_HINT));

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
