#include "ProjectStickActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
}

void ProjectStickActivity::onEnter() {
  Activity::onEnter();
  service.begin();
  state = State::Connecting;
  setStatus(tr(STR_PROJECT_STICK_CONNECTING));
  requestUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    workPending = true;
  } else {
    launchWifiSelection();
  }
}

void ProjectStickActivity::launchWifiSelection() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    state = State::Error;
    setStatus(tr(STR_MEMORY_ERROR));
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      updateState(service.refreshScheduledContent() ? ProjectStickService::SyncResult::OfflineCache
                                                    : ProjectStickService::SyncResult::Failed);
      return;
    }
    state = State::Connecting;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    workPending = true;
  });
}

void ProjectStickActivity::runInitialSync() {
  requestUpdateAndWait();
  updateState(service.sync());
  lastManifestPollMs = millis();
  lastAlertPollMs = millis();
  lastScheduleCheckMs = millis();
  lastRegisterMs = millis();
}

void ProjectStickActivity::updateState(ProjectStickService::SyncResult result) {
  switch (result) {
    case ProjectStickService::SyncResult::Updated:
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_UPDATED));
      break;
    case ProjectStickService::SyncResult::Unchanged:
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_ONLINE));
      break;
    case ProjectStickService::SyncResult::OfflineCache:
      state = State::Offline;
      setStatus(tr(STR_PROJECT_STICK_OFFLINE));
      break;
    case ProjectStickService::SyncResult::NoContent:
      state = State::Empty;
      setStatus(tr(STR_PROJECT_STICK_NO_CONTENT));
      break;
    case ProjectStickService::SyncResult::Inactive:
      state = State::Inactive;
      setStatus(tr(STR_PROJECT_STICK_INACTIVE));
      break;
    case ProjectStickService::SyncResult::Failed:
      state = State::Error;
      setStatus(tr(STR_PROJECT_STICK_FAILED));
      break;
  }
  requestUpdate();
}

void ProjectStickActivity::loop() {
  if (workPending) {
    workPending = false;
    runInitialSync();
    return;
  }

  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) longPressFired = false;
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    if (service.display().copyId == 0) {
      service.sendManualRefresh();
    } else {
      service.sendFeedback(false);
      setStatus(tr(STR_PROJECT_STICK_MEH_SENT));
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(true);
      setStatus(tr(STR_PROJECT_STICK_USEFUL_SENT));
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    setStatus(tr(STR_PROJECT_STICK_REFRESHING));
    requestUpdateAndWait();
    service.sendManualRefresh();
    state = service.display().copyId == 0 ? State::Empty : State::Online;
    setStatus(service.display().copyId == 0 ? tr(STR_PROJECT_STICK_NO_CONTENT) : tr(STR_PROJECT_STICK_ONLINE));
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::PROJECT_STICK);
    return;
  }

  const uint32_t nowMs = millis();
  if (state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastManifestPollMs >= service.pollIntervalSeconds() * 1000UL) {
    lastManifestPollMs = nowMs;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    requestUpdateAndWait();
    const bool heartbeatDue = nowMs - lastRegisterMs >= 4UL * 60UL * 60UL * 1000UL;
    updateState(service.sync(heartbeatDue));
    if (heartbeatDue) lastRegisterMs = millis();
    return;
  }
  if (nowMs - lastScheduleCheckMs >= 30000UL) {
    lastScheduleCheckMs = nowMs;
    if (service.refreshIfScheduleChanged()) {
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_ONLINE));
      requestUpdate();
    }
  }
  if (state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastAlertPollMs >= service.alertPollIntervalSeconds() * 1000UL) {
    lastAlertPollMs = nowMs;
    if (service.pollAlerts()) {
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_ALERT));
      requestUpdate();
    }
  }
}

void ProjectStickActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_PROJECT_STICK));

  const auto& display = service.display();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int y = contentTop;
  if (!display.scenario.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, display.scenario.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
  }

  if (!display.text.empty()) {
    const auto lines = renderer.wrappedText(UI_12_FONT_ID, display.text.c_str(),
                                            width - metrics.contentSidePadding * 2, 6);
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += renderer.getLineHeight(UI_12_FONT_ID) + 3;
    }
    if (!display.tone.empty()) {
      y += 6;
      renderer.drawCenteredText(SMALL_FONT_ID, y, display.tone.c_str());
    }
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, height / 2 - 10, statusLine, true, EpdFontFamily::BOLD);
  }

  char diagnostics[96];
  snprintf(diagnostics, sizeof(diagnostics), "%.54s  v%lu  %s:%lu", statusLine,
           static_cast<unsigned long>(service.activeVersion()), tr(STR_PROJECT_STICK_QUEUE),
           static_cast<unsigned long>(service.pendingEventCount()));
  renderer.drawCenteredText(SMALL_FONT_ID, height - metrics.buttonHintsHeight - 26, diagnostics);

  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_PROJECT_STICK_USEFUL_HOLD_MEH), "", tr(STR_PROJECT_STICK_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ProjectStickActivity::setStatus(const char* text) {
  snprintf(statusLine, sizeof(statusLine), "%s", text ? text : "");
}
