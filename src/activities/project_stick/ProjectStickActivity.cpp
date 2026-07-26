#include "ProjectStickActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int BLOCK_GAP = 10;
constexpr int BODY_LINE_GAP = 4;
constexpr int DIAGNOSTICS_VERTICAL_PADDING = 6;
constexpr int X3_SIDE_HINT_BOTTOM = 235;
}  // namespace

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

  // X3 side buttons are fixed physical controls: BTN_UP is on the left edge
  // and BTN_DOWN is on the right edge.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(false);
      setStatus(tr(STR_PROJECT_STICK_MEH_SENT));
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(true);
      setStatus(tr(STR_PROJECT_STICK_USEFUL_SENT));
      requestUpdate();
    }
    return;
  }

  // Project.Stick intentionally uses fixed physical front-button positions:
  // bottom-left pair = Back / Refresh; bottom-right pair = unassigned.
  const int frontButton = mappedInput.getPressedFrontButton();
  if (frontButton == HalGPIO::BTN_BACK) {
    onGoHome(HomeMenuItem::PROJECT_STICK);
    return;
  }
  if (frontButton == HalGPIO::BTN_CONFIRM) {
    setStatus(tr(STR_PROJECT_STICK_REFRESHING));
    requestUpdateAndWait();
    service.sendManualRefresh();
    state = service.display().copyId == 0 ? State::Empty : State::Online;
    setStatus(service.display().copyId == 0 ? tr(STR_PROJECT_STICK_NO_CONTENT) : tr(STR_PROJECT_STICK_ONLINE));
    requestUpdate();
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
  const int hintTop = height - metrics.buttonHintsHeight;
  const int diagnosticsLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int diagnosticsHeight = diagnosticsLineHeight + DIAGNOSTICS_VERTICAL_PADDING * 2;
  const int diagnosticsTop = hintTop - diagnosticsHeight;
  const int contentInset = metrics.contentSidePadding;
  const Rect contentBounds{contentInset, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing,
                           width - contentInset * 2,
                           diagnosticsTop - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing)};

  if (!display.text.empty()) {
    const int scenarioHeight =
        display.scenario.empty()
            ? 0
            : renderer.getTextLineHeight(UI_10_FONT_ID, display.scenario.c_str(), EpdFontFamily::BOLD);
    const int toneHeight =
        display.tone.empty() ? 0 : renderer.getTextLineHeight(SMALL_FONT_ID, display.tone.c_str());
    const int scenarioReserve = scenarioHeight > 0 ? scenarioHeight + BLOCK_GAP : 0;
    const int toneReserve = toneHeight > 0 ? toneHeight + BLOCK_GAP : 0;
    const int bodyLineHeight =
        renderer.getTextLineHeight(UI_12_FONT_ID, display.text.c_str(), EpdFontFamily::BOLD);
    const int bodyLineStep = bodyLineHeight + BODY_LINE_GAP;
    const int safeContentTop = std::max(contentBounds.y, X3_SIDE_HINT_BOTTOM + BLOCK_GAP);
    const int safeContentHeight = contentBounds.y + contentBounds.height - safeContentTop;
    const int maxBodyLines =
        std::max(1, (safeContentHeight - scenarioReserve - toneReserve + BODY_LINE_GAP) / bodyLineStep);
    const auto lines = renderer.wrappedCjkText(UI_12_FONT_ID, display.text.c_str(), contentBounds.width, maxBodyLines,
                                               EpdFontFamily::BOLD);
    const int bodyHeight =
        lines.empty() ? 0 : static_cast<int>(lines.size()) * bodyLineStep - BODY_LINE_GAP;
    const int centeredBodyY = contentBounds.y + (contentBounds.height - bodyHeight) / 2;
    const int minBodyY = safeContentTop + scenarioReserve;
    const int maxBodyY = contentBounds.y + contentBounds.height - toneReserve - bodyHeight;
    const int bodyY = std::clamp(centeredBodyY, minBodyY, std::max(minBodyY, maxBodyY));

    if (!display.scenario.empty()) {
      UITheme::drawCenteredText(renderer, contentBounds, UI_10_FONT_ID, bodyY - scenarioReserve,
                                display.scenario.c_str(), true,
                                EpdFontFamily::BOLD);
    }
    int y = bodyY;
    for (const auto& line : lines) {
      UITheme::drawCenteredText(renderer, contentBounds, UI_12_FONT_ID, y, line.c_str(), true,
                                EpdFontFamily::BOLD);
      y += bodyLineStep;
    }
    if (!display.tone.empty()) {
      UITheme::drawCenteredText(renderer, contentBounds, SMALL_FONT_ID, bodyY + bodyHeight + BLOCK_GAP,
                                display.tone.c_str());
    }
  } else {
    UITheme::drawCenteredWrappedText(renderer, contentBounds, UI_12_FONT_ID, statusLine, 4, true,
                                     EpdFontFamily::BOLD);
  }

  char diagnostics[96];
  snprintf(diagnostics, sizeof(diagnostics), "%.54s  v%lu  %s:%lu", statusLine,
           static_cast<unsigned long>(service.activeVersion()), tr(STR_PROJECT_STICK_QUEUE),
           static_cast<unsigned long>(service.pendingEventCount()));
  renderer.drawLine(0, diagnosticsTop, width - 1, diagnosticsTop);
  const auto fittedDiagnostics =
      renderer.truncatedText(SMALL_FONT_ID, diagnostics, width - metrics.contentSidePadding * 2);
  renderer.drawCenteredText(SMALL_FONT_ID, diagnosticsTop + DIAGNOSTICS_VERTICAL_PADDING,
                            fittedDiagnostics.c_str());

  GUI.drawSideButtonHints(renderer, tr(STR_PROJECT_STICK_NOT_USEFUL), tr(STR_PROJECT_STICK_USEFUL));
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_PROJECT_STICK_REFRESH), "", "");
  renderer.displayBuffer();
}

void ProjectStickActivity::setStatus(const char* text) {
  snprintf(statusLine, sizeof(statusLine), "%s", text ? text : "");
}
