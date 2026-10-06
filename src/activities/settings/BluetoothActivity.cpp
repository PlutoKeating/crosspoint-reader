#include "BluetoothActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "project_stick/StudioBluetooth.h"

namespace {
const char* radioLabel(const studio_ble::Radio radio) {
  switch (radio) {
    case studio_ble::Radio::Off:
      return tr(STR_BT_STATE_OFF);
    case studio_ble::Radio::Advertising:
      return tr(STR_BT_STATE_ADVERTISING);
    case studio_ble::Radio::Connected:
      return tr(STR_BT_STATE_CONNECTED);
    case studio_ble::Radio::Paused:
      return tr(STR_BT_STATE_PAUSED);
    case studio_ble::Radio::Failed:
      return tr(STR_BT_STATE_FAILED);
    case studio_ble::Radio::Idle:
    default:
      return tr(STR_BT_STATE_IDLE);
  }
}

void formatAgo(char* out, const size_t size, const int32_t seconds) {
  if (seconds < 0)
    snprintf(out, size, "%s", tr(STR_BT_NEVER));
  else if (seconds < 120)
    snprintf(out, size, tr(STR_BT_SECONDS_AGO), static_cast<int>(seconds));
  else
    snprintf(out, size, tr(STR_BT_MINUTES_AGO), static_cast<int>(seconds / 60));
}
}  // namespace

void BluetoothActivity::onEnter() {
  Activity::onEnter();
  renderedGeneration = studio_ble::link().generation;
  requestUpdate();
}

void BluetoothActivity::loop() {
  const uint32_t generation = studio_ble::link().generation;
  if (generation != renderedGeneration) {
    renderedGeneration = generation;
    requestUpdate();
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;
  if (backPressSeen && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const bool enable = !studio_ble::enabled();
    SETTINGS.bluetoothEnabled = enable ? 1 : 0;
    SETTINGS.saveToFile();
    studio_ble::setEnabled(enable);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    studio_ble::restart();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) requestUpdate();
}

void BluetoothActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int side = metrics.contentSidePadding;
  // Noto Sans SC throughout: rows mix Chinese labels with Latin values, and
  // one family keeps their sizes and baselines aligned.
  constexpr int ROW_FONT = NOTOSANSSC_12_FONT_ID;
  constexpr int HELP_FONT = NOTOSANSSC_10_FONT_ID;
  const int rowHeight = renderer.getTextLineHeight(ROW_FONT, tr(STR_BT_STATUS)) + metrics.verticalSpacing;
  const auto info = studio_ble::diagnostics();
  const bool on = info.link.radio != studio_ble::Radio::Off;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BLUETOOTH),
                 on ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  // The state is the answer to "why can't my phone find it": one bold line.
  const char* state = radioLabel(info.link.radio);
  renderer.drawText(NOTOSANSSC_13_FONT_ID, side, y, state, true, EpdFontFamily::BOLD);
  y += renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, state) + metrics.verticalSpacing * 2;

  const int valueX = side + (pageWidth - side * 2) * 2 / 5;
  const auto row = [&](const char* label, const char* value) {
    renderer.drawText(ROW_FONT, side, y, label, true);
    const std::string fitted = renderer.truncatedText(ROW_FONT, value, pageWidth - side - valueX);
    renderer.drawText(ROW_FONT, valueX, y, fitted.c_str(), true);
    y += rowHeight;
  };
  char text[64];
  row(tr(STR_BT_MODE), info.link.setupMode ? tr(STR_BT_MODE_SETUP) : tr(STR_BT_MODE_BOUND));
  row(tr(STR_BT_NAME), info.name.empty() ? "-" : info.name.c_str());
  row(tr(STR_BT_ADDRESS), info.address.empty() ? "-" : info.address.c_str());
  if (!info.error.empty()) {
    snprintf(text, sizeof(text), "%s (%d)", info.error.c_str(), info.errorCode);
    row(tr(STR_BT_ERROR), text);
  }
  snprintf(text, sizeof(text), tr(STR_BT_STARTS_FORMAT), static_cast<unsigned>(info.starts),
           static_cast<unsigned>(info.startFailures));
  row(tr(STR_BT_STARTS), text);
  snprintf(text, sizeof(text), "%u", static_cast<unsigned>(info.connections));
  row(tr(STR_BT_CONNECTIONS), text);
  formatAgo(text, sizeof(text), info.sinceConnect);
  row(tr(STR_BT_LAST_CONNECT), text);
  // Every connect the stack saw, including link-level failures and refusals,
  // which "连接次数" does not count (2.7.7).
  if (info.linkFailures) {
    snprintf(text, sizeof(text), tr(STR_BT_ATTEMPTS_FAILED_FORMAT), static_cast<unsigned>(info.linkAttempts),
             static_cast<unsigned>(info.linkFailures), info.lastLinkFailure,
             static_cast<unsigned>(info.lastLinkFailureHeap / 1024));
  } else {
    snprintf(text, sizeof(text), tr(STR_BT_ATTEMPTS_FORMAT), static_cast<unsigned>(info.linkAttempts),
             static_cast<unsigned>(info.linkRejected));
  }
  row(tr(STR_BT_ATTEMPTS), text);
  if (info.lastDisconnectReason >= 0) {
    snprintf(text, sizeof(text), "0x%x", info.lastDisconnectReason);
    row(tr(STR_BT_LAST_DISCONNECT), text);
  }
  row("Wi-Fi", WiFi.status() == WL_CONNECTED ? tr(STR_PROJECT_STICK_STATUS_ONLINE) : tr(STR_PROJECT_STICK_STATUS_OFFLINE));
  snprintf(text, sizeof(text), "%u / %u KB", static_cast<unsigned>(ESP.getFreeHeap() / 1024),
           static_cast<unsigned>(ESP.getMaxAllocHeap() / 1024));
  row(tr(STR_BT_MEMORY), text);
  if (info.hostStackFree) {
    snprintf(text, sizeof(text), "%u B", static_cast<unsigned>(info.hostStackFree));
    row(tr(STR_BT_HOST_STACK), text);
  }

  y += metrics.verticalSpacing * 2;
  renderer.drawLine(side, y, pageWidth - side, y, 1, true);
  y += metrics.verticalSpacing * 2;
  const char* help = on ? tr(STR_BT_HELP) : tr(STR_BT_HELP_OFF);
  const int helpLine = renderer.getTextLineHeight(HELP_FONT, help);
  const int hintTop = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  for (const auto& line : renderer.wrappedCjkText(HELP_FONT, help, pageWidth - side * 2, 5)) {
    if (y + helpLine > hintTop) break;
    renderer.drawText(HELP_FONT, side, y, line.c_str(), true);
    y += helpLine + 2;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), on ? tr(STR_BT_TURN_OFF) : tr(STR_BT_TURN_ON),
                                            tr(STR_BT_REFRESH), on ? tr(STR_BT_RESTART) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
