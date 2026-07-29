#include "ProjectStickActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/icons/project_stick_icons.h"
#include "fontIds.h"
#include "ProjectStickCore.h"

namespace {
constexpr int BODY_LINE_GAP = 4;
constexpr int SIDE_BUTTON_MARGIN = 4;
constexpr int SIDE_BUTTON_WIDTH = 30;
constexpr int SIDE_BUTTON_HEIGHT = 80;
constexpr int SIDE_BUTTON_Y = 155;
constexpr int SIDE_CONTENT_GAP = 8;
constexpr int SCENARIO_TAG_HORIZONTAL_PADDING = 16;
constexpr int SCENARIO_TAG_VERTICAL_PADDING = 4;
constexpr int GOLDEN_RATIO_NUMERATOR = 1618;
constexpr int GOLDEN_RATIO_DENOMINATOR = 1000;
constexpr int NETWORK_TAG_BOTTOM_GAP = 12;
constexpr int NETWORK_TAG_HORIZONTAL_PADDING = 8;
constexpr int NETWORK_TAG_DOT_SIZE = 5;
constexpr int NETWORK_TAG_DOT_GAP = 5;
constexpr int COMPACT_HEADER_BATTERY_RESERVE = 90;

void drawFeedbackIcon(const GfxRenderer& renderer, int x, int y, bool thumbsUp) {
  constexpr int size = 24;
  constexpr int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const uint8_t byte = ProjectStickFeedback24Icon.bits[row * rowBytes + (col >> 3)];
      const bool ink = ((byte >> (7 - (col & 7))) & 1) == 0;
      if (ink) {
        const int screenY = thumbsUp ? y + row : y + size - 1 - row;
        renderer.drawPixel(x + col, screenY, true);
      }
    }
  }
}

void drawFeedbackHints(const GfxRenderer& renderer) {
  const int width = renderer.getScreenWidth();
  const int iconOffsetX = (SIDE_BUTTON_WIDTH - ProjectStickFeedback24Icon.w) / 2;
  const int iconOffsetY = (SIDE_BUTTON_HEIGHT - ProjectStickFeedback24Icon.h) / 2;
  const int leftX = SIDE_BUTTON_MARGIN;
  const int rightX = width - SIDE_BUTTON_MARGIN - SIDE_BUTTON_WIDTH;

  renderer.drawRoundedRect(leftX, SIDE_BUTTON_Y, SIDE_BUTTON_WIDTH, SIDE_BUTTON_HEIGHT, 1, 8, true);
  renderer.drawRoundedRect(rightX, SIDE_BUTTON_Y, SIDE_BUTTON_WIDTH, SIDE_BUTTON_HEIGHT, 1, 8, true);
  drawFeedbackIcon(renderer, leftX + iconOffsetX, SIDE_BUTTON_Y + iconOffsetY, false);
  drawFeedbackIcon(renderer, rightX + iconOffsetX, SIDE_BUTTON_Y + iconOffsetY, true);
}

void drawNetworkStatusTag(const GfxRenderer& renderer, const Rect& headerBounds, int rightInset,
                          bool connected) {
  const char* label =
      connected ? tr(STR_PROJECT_STICK_STATUS_ONLINE) : tr(STR_PROJECT_STICK_STATUS_OFFLINE);
  const int textHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int tagHeight = textHeight + 4;
  const int tagWidth = NETWORK_TAG_HORIZONTAL_PADDING * 2 + NETWORK_TAG_DOT_SIZE +
                       NETWORK_TAG_DOT_GAP + renderer.getTextWidth(SMALL_FONT_ID, label);
  const bool hasSecondHeaderRow = headerBounds.height >= 60;
  const int safeRightInset =
      hasSecondHeaderRow ? rightInset : std::max(rightInset, COMPACT_HEADER_BATTERY_RESERVE);
  const int tagX = renderer.getScreenWidth() - safeRightInset - tagWidth;
  const int tagY =
      hasSecondHeaderRow
          ? headerBounds.y + headerBounds.height - tagHeight - NETWORK_TAG_BOTTOM_GAP
          : headerBounds.y + (headerBounds.height - tagHeight) / 2;
  const int dotX = tagX + NETWORK_TAG_HORIZONTAL_PADDING;
  const int dotY = tagY + (tagHeight - NETWORK_TAG_DOT_SIZE) / 2;

  renderer.drawRoundedRect(tagX, tagY, tagWidth, tagHeight, 1, tagHeight / 2, true);
  if (connected) {
    renderer.fillRect(dotX, dotY, NETWORK_TAG_DOT_SIZE, NETWORK_TAG_DOT_SIZE);
  } else {
    renderer.drawRect(dotX, dotY, NETWORK_TAG_DOT_SIZE, NETWORK_TAG_DOT_SIZE);
  }
  renderer.drawText(SMALL_FONT_ID, dotX + NETWORK_TAG_DOT_SIZE + NETWORK_TAG_DOT_GAP, tagY + 2,
                    label);
}

const char* scenarioDisplayName(const std::string& scenario) {
  if (scenario == "pre_open") return tr(STR_PROJECT_STICK_SCENARIO_PRE_OPEN);
  if (scenario == "midday_reset") return tr(STR_PROJECT_STICK_SCENARIO_MIDDAY_RESET);
  if (scenario == "post_close") return tr(STR_PROJECT_STICK_SCENARIO_POST_CLOSE);
  if (scenario == "volatility_alert") return tr(STR_PROJECT_STICK_SCENARIO_VOLATILITY_ALERT);
  if (scenario == "manual_refresh") return tr(STR_PROJECT_STICK_SCENARIO_MANUAL_REFRESH);
  return scenario.c_str();
}
}  // namespace

void ProjectStickActivity::onEnter() {
  Activity::onEnter();
  service.begin();
  if (WiFi.status() == WL_CONNECTED) {
    state = State::Connecting;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    workPending = true;
  } else {
    service.refreshScheduledContent();
    state = State::Offline;
    setStatus(tr(STR_PROJECT_STICK_OFFLINE));
  }
  requestUpdate();
}

void ProjectStickActivity::runInitialSync() {
  requestUpdateAndWait();
  const auto report = service.sync();
  recordSyncTiming(report);
  updateState(report);
  lastManifestAttemptMs = millis();
  lastAlertPollMs = millis();
  lastScheduleCheckMs = millis();
}

void ProjectStickActivity::recordSyncTiming(const project_stick::SyncReport& report) {
  const uint32_t nowMs = millis();
  if (project_stick::shouldRecordRegisterSuccess(report)) lastRegisterMs = nowMs;
  if (project_stick::shouldRecordManifestPoll(report)) lastManifestPollMs = nowMs;
}

void ProjectStickActivity::updateState(const project_stick::SyncReport& report) {
  lastSyncReport = report;
  switch (report.result) {
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
      service.sendFeedback(false, WiFi.status() == WL_CONNECTED);
      setStatus(tr(STR_PROJECT_STICK_MEH_SENT));
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(true, WiFi.status() == WL_CONNECTED);
      setStatus(tr(STR_PROJECT_STICK_USEFUL_SENT));
      requestUpdate();
    }
    return;
  }

  // Project.Stick intentionally uses fixed physical front-button positions:
  // Back / Wi-Fi / unassigned / Refresh.
  const int frontButton = mappedInput.getPressedFrontButton();
  if (frontButton == HalGPIO::BTN_BACK) {
    onGoHome(HomeMenuItem::PROJECT_STICK);
    return;
  }
  if (frontButton == HalGPIO::BTN_CONFIRM) {
    launchWifiSelection();
    return;
  }
  if (frontButton == HalGPIO::BTN_RIGHT) {
    setStatus(tr(STR_PROJECT_STICK_REFRESHING));
    requestUpdateAndWait();
    const bool online = WiFi.status() == WL_CONNECTED;
    if (project_stick::manualRefreshMode(online, service.activeVersion(), lastSyncReport) ==
        project_stick::ManualRefreshMode::FullCloudSync) {
      service.queueManualRefresh();
      const auto report = service.sync(true);
      lastManifestAttemptMs = millis();
      recordSyncTiming(report);
      updateState(report);
    } else if (service.sendManualRefresh(online)) {
      state = online ? State::Online : State::Offline;
      setStatus(online ? tr(STR_PROJECT_STICK_ONLINE) : tr(STR_PROJECT_STICK_OFFLINE));
    } else {
      state = online ? State::Error : State::Offline;
      setStatus(online ? tr(STR_PROJECT_STICK_FAILED) : tr(STR_PROJECT_STICK_OFFLINE));
    }
    requestUpdate();
    return;
  }

  const uint32_t nowMs = millis();
  if (state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastManifestAttemptMs >= service.pollIntervalSeconds() * 1000UL) {
    lastManifestAttemptMs = nowMs;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    requestUpdateAndWait();
    const bool heartbeatDue = project_stick::registrationDue(nowMs, lastRegisterMs);
    const auto report = service.sync(heartbeatDue);
    recordSyncTiming(report);
    updateState(report);
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

void ProjectStickActivity::launchWifiSelection() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
      [this](const ActivityResult&) {
        if (WiFi.status() == WL_CONNECTED) {
          state = State::Connecting;
          setStatus(tr(STR_PROJECT_STICK_SYNCING));
          workPending = true;
        } else {
          service.refreshScheduledContent();
          state = State::Offline;
          setStatus(tr(STR_PROJECT_STICK_OFFLINE));
        }
      });
}

void ProjectStickActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  const Rect headerBounds{0, metrics.topPadding, width, metrics.headerHeight};
  GUI.drawHeader(renderer, headerBounds, tr(STR_PROJECT_STICK));
  drawNetworkStatusTag(renderer, headerBounds, metrics.contentSidePadding,
                       WiFi.status() == WL_CONNECTED);

  const auto& display = service.display();
  const int hintTop = height - metrics.buttonHintsHeight;
  const int contentInset =
      std::max(metrics.contentSidePadding, SIDE_BUTTON_MARGIN + SIDE_BUTTON_WIDTH + SIDE_CONTENT_GAP);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect contentBounds{contentInset, contentTop, width - contentInset * 2,
                           hintTop - metrics.verticalSpacing - contentTop};

  if (!display.text.empty()) {
    const std::string bodyText = project_stick::stripWrappingQuotes(display.text);
    const char* scenarioName = scenarioDisplayName(display.scenario);
    const int scenarioHeight =
        scenarioName[0] == '\0'
            ? 0
            : renderer.getTextLineHeight(NOTOSANSSC_12_FONT_ID, scenarioName);
    const int tagHeight =
        scenarioHeight > 0 ? scenarioHeight + SCENARIO_TAG_VERTICAL_PADDING * 2 : 0;
    const int tagWidth =
        scenarioHeight > 0
            ? renderer.getTextWidth(NOTOSANSSC_12_FONT_ID, scenarioName) +
                  SCENARIO_TAG_HORIZONTAL_PADDING * 2
            : 0;
    const int bodyTagGap =
        scenarioHeight > 0
            ? std::max(metrics.verticalSpacing,
                       tagHeight * GOLDEN_RATIO_DENOMINATOR / GOLDEN_RATIO_NUMERATOR)
            : 0;
    const int maxBodyHeight = std::max(1, contentBounds.height - tagHeight - bodyTagGap);
    const int bodyLineHeight =
        renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, bodyText.c_str());
    const int bodyLineStep = bodyLineHeight + BODY_LINE_GAP;
    const int maxBodyLines =
        std::max(1, (maxBodyHeight + BODY_LINE_GAP) / bodyLineStep);
    const auto lines =
        renderer.wrappedCjkText(NOTOSANSSC_13_FONT_ID, bodyText.c_str(), contentBounds.width, maxBodyLines);
    const int bodyHeight =
        lines.empty() ? 0 : static_cast<int>(lines.size()) * bodyLineStep - BODY_LINE_GAP;
    const int groupHeight = bodyHeight + bodyTagGap + tagHeight;
    const int freeHeight = std::max(0, contentBounds.height - groupHeight);
    const int bodyY =
        contentBounds.y +
        freeHeight * (GOLDEN_RATIO_NUMERATOR - GOLDEN_RATIO_DENOMINATOR) /
            GOLDEN_RATIO_NUMERATOR;
    const Rect bodyBounds{contentBounds.x, bodyY, contentBounds.width, bodyHeight};

    int y = bodyY;
    for (const auto& line : lines) {
      UITheme::drawCenteredText(renderer, bodyBounds, NOTOSANSSC_13_FONT_ID, y, line.c_str(), true);
      y += bodyLineStep;
    }
    if (scenarioHeight > 0) {
      const int tagX = (width - tagWidth) / 2;
      const int tagY = bodyY + bodyHeight + bodyTagGap;
      renderer.drawRoundedRect(tagX, tagY, tagWidth, tagHeight, 1, tagHeight / 2, true);
      const Rect tagBounds{tagX, tagY, tagWidth, tagHeight};
      UITheme::drawCenteredText(renderer, tagBounds, NOTOSANSSC_12_FONT_ID,
                                tagY + SCENARIO_TAG_VERTICAL_PADDING, scenarioName, true);
    }
  } else {
    const bool showOfflineEmptyState = WiFi.status() != WL_CONNECTED && state == State::Offline;
    if (showOfflineEmptyState) {
      const char* title = tr(STR_PROJECT_STICK_NO_LOCAL_CONTENT);
      const char* helper = tr(STR_PROJECT_STICK_CONNECT_TO_SYNC);
      const int titleHeight =
          renderer.getTextLineHeight(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
      const int helperHeight = renderer.getTextLineHeight(UI_10_FONT_ID, helper);
      constexpr int emptyStateGap = 12;
      const int groupHeight = titleHeight + emptyStateGap + helperHeight;
      const int groupY = contentBounds.y + (contentBounds.height - groupHeight) / 2;
      UITheme::drawCenteredText(renderer, contentBounds, UI_12_FONT_ID, groupY, title, true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, contentBounds, UI_10_FONT_ID,
                                groupY + titleHeight + emptyStateGap, helper, true);
    } else {
      UITheme::drawCenteredWrappedText(renderer, contentBounds, UI_12_FONT_ID, statusLine, 4, true,
                                       EpdFontFamily::BOLD);
    }
  }

  drawFeedbackHints(renderer);
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_PROJECT_STICK_CONNECT_WIFI), "",
                      tr(STR_PROJECT_STICK_REFRESH));
  renderer.displayBuffer();
}

void ProjectStickActivity::setStatus(const char* text) {
  snprintf(statusLine, sizeof(statusLine), "%s", text ? text : "");
}
