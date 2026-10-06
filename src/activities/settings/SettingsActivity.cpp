#include "SettingsActivity.h"

#include <GfxRenderer.h>

#include "BluetoothActivity.h"
#include "CrossPointSettings.h"
#include "FirmwareUpdateActivity.h"
#include "LanguageSelectActivity.h"
#include "MappedInputManager.h"
#include "SdFirmwareUpdateActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"

const SettingsActivity::Item SettingsActivity::items[ITEM_COUNT] = {Item::Network, Item::Bluetooth,
                                                                    Item::FirmwareUpdate, Item::SdFirmwareUpdate,
                                                                    Item::Language};

StrId SettingsActivity::itemName(const Item item) {
  switch (item) {
    case Item::Network:
      return StrId::STR_WIFI_NETWORKS;
    case Item::Bluetooth:
      return StrId::STR_BLUETOOTH;
    case Item::FirmwareUpdate:
      return StrId::STR_OTA_TITLE;
    case Item::SdFirmwareUpdate:
      return StrId::STR_SD_FIRMWARE_UPDATE;
    case Item::Language:
      return StrId::STR_LANGUAGE;
  }
  return StrId::STR_NONE_OPT;
}

void SettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void SettingsActivity::openSelected() {
  if (selectedIndex < 0 || selectedIndex >= ITEM_COUNT) return;
  auto onResult = [](const ActivityResult&) { SETTINGS.saveToFile(); };
  switch (items[selectedIndex]) {
    case Item::Network:
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), onResult);
      break;
    case Item::Bluetooth:
      startActivityForResult(std::make_unique<BluetoothActivity>(renderer, mappedInput), onResult);
      break;
    case Item::FirmwareUpdate:
      startActivityForResult(std::make_unique<FirmwareUpdateActivity>(renderer, mappedInput), onResult);
      break;
    case Item::SdFirmwareUpdate:
      startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), onResult);
      break;
    case Item::Language:
      startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), onResult);
      break;
  }
}

void SettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoHome();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    openSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  switch (handleListTouch(selectedIndex, ITEM_COUNT, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      openSelected();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ITEM_COUNT);
    requestUpdate();
  });
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ITEM_COUNT, selectedIndex,
      [](int index) { return std::string(I18N.get(itemName(items[index]))); }, nullptr, nullptr, nullptr, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
