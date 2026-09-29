#include "FirmwarePickerActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"

void FirmwarePickerActivity::scanDirectory(const char* directory) {
  auto dir = Storage.open(directory);
  if (!dir || !dir.isDirectory()) return;
  char name[128];
  for (auto file = dir.openNextFile(); file && paths.size() < MAX_ENTRIES; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, sizeof(name));
    if (name[0] == '.' || !FsHelpers::checkFileExtension(std::string_view{name}, ".bin")) continue;
    std::string path = directory;
    if (path.back() != '/') path += '/';
    path += name;
    paths.push_back(std::move(path));
  }
}

void FirmwarePickerActivity::onEnter() {
  Activity::onEnter();
  paths.reserve(MAX_ENTRIES);
  scanDirectory("/");
  scanDirectory("/firmware");
  std::sort(paths.begin(), paths.end());
  selectedIndex = 0;
  LOG_INF("FW", "Firmware picker found %u image(s)", static_cast<unsigned>(paths.size()));
  requestUpdate();
}

void FirmwarePickerActivity::onExit() {
  Activity::onExit();
  paths.clear();
  paths.shrink_to_fit();
}

void FirmwarePickerActivity::choose() {
  if (paths.empty()) return;
  setResult(FilePathResult{paths[selectedIndex]});
  finish();
}

void FirmwarePickerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    choose();
    return;
  }

  const int count = static_cast<int>(paths.size());
  if (count == 0) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  switch (handleListTouch(selectedIndex, count, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      choose();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

void FirmwarePickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SD_FIRMWARE_UPDATE));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (paths.empty()) {
    GUI.drawHelpText(renderer, Rect{0, contentTop, pageWidth, contentHeight}, tr(STR_NO_BIN_FILES));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(paths.size()), selectedIndex,
        [this](int index) { return paths[index]; }, nullptr, nullptr, nullptr, false);
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), paths.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
