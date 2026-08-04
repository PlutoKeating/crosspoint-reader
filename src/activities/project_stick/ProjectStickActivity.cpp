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
constexpr uint32_t FEEDBACK_BUBBLE_FRAME_MS = 120;
constexpr uint32_t FEEDBACK_BUBBLE_VISIBLE_MS = 2200;
constexpr uint8_t FEEDBACK_BUBBLE_FINAL_FRAME = 3;

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
  if (scenario == "market_open") return tr(STR_PROJECT_STICK_SCENARIO_MARKET_OPEN);
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
  PROJECT_STICK_BACKGROUND_SYNC.begin();
  backgroundResultSequence = PROJECT_STICK_BACKGROUND_SYNC.latestSequence();
  const bool haveLocalContent = service.refreshScheduledContent();
  if (WiFi.status() == WL_CONNECTED) {
    state = haveLocalContent ? State::Online : State::Connecting;
    setStatus(haveLocalContent ? tr(STR_PROJECT_STICK_ONLINE)
                               : tr(STR_PROJECT_STICK_SYNCING));
    requestCloudSync(true);
  } else {
    state = State::Offline;
    setStatus(tr(STR_PROJECT_STICK_OFFLINE));
  }
  requestUpdate();
}

bool ProjectStickActivity::requestCloudSync(bool registerFirst) {
  if (WiFi.status() != WL_CONNECTED) return false;
  return PROJECT_STICK_BACKGROUND_SYNC.requestSync(registerFirst);
}

void ProjectStickActivity::runManualRefresh() {
  const bool online = WiFi.status() == WL_CONNECTED;
  const bool selected = service.sendManualRefresh(false);
  if (selected) {
    state = online ? State::Online : State::Offline;
    setStatus(online ? tr(STR_PROJECT_STICK_ONLINE) : tr(STR_PROJECT_STICK_OFFLINE));
  } else if (online) {
    state = State::Connecting;
    setStatus(tr(STR_PROJECT_STICK_REFRESHING));
  } else {
    state = State::Offline;
    setStatus(tr(STR_PROJECT_STICK_OFFLINE));
  }
  // Wake the renderer immediately. Cloud work is only offered to the
  // process-lifetime worker after the local selection is complete.
  requestUpdate(true);
  requestCloudSync(project_stick::registrationDue(millis(), lastRegisterMs));
}

void ProjectStickActivity::applyBackgroundResult() {
  ProjectStickBackgroundSync::Result result;
  if (!PROJECT_STICK_BACKGROUND_SYNC.takeResult(backgroundResultSequence, result)) return;

  if (result.kind == ProjectStickBackgroundSync::WorkKind::AlertPoll) {
    if (result.alertReceived) {
      service.adoptDisplay(std::move(result.alertDisplay));
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_ALERT));
      requestUpdate();
    }
    return;
  }

  const auto& report = result.syncReport;
  service.adoptServerTime(report.synchronizedAt);
  recordSynchronizedAt(report.synchronizedAt);
  recordSyncTiming(report);
  lastManifestAttemptMs = millis();
  lastAlertPollMs = millis();
  lastScheduleCheckMs = millis();
  if ((report.result == ProjectStickService::SyncResult::Updated ||
       report.result == ProjectStickService::SyncResult::Unchanged) &&
      (report.result == ProjectStickService::SyncResult::Updated ||
       service.display().copyId == 0)) {
    service.refreshScheduledContent();
  }
  updateState(report);
#ifdef SIMULATOR
  simulatorRecoveryPending =
      report.result == ProjectStickService::SyncResult::Failed &&
      std::getenv("CROSSPOINT_SIM_RECOVER_AFTER_FIRST_FAILURE") != nullptr;
  simulatorAlertPollPending =
      (report.result == ProjectStickService::SyncResult::Updated ||
       report.result == ProjectStickService::SyncResult::Unchanged) &&
      std::getenv("CROSSPOINT_SIM_POLL_ALERT_ONCE") != nullptr;
#endif
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
  applyBackgroundResult();
  updateFeedbackBubble();
#ifdef SIMULATOR
  if (simulatorRecoveryPending) {
    simulatorRecoveryPending = false;
    LOG_INF("STICK", "Simulator invoking the Project.Stick Refresh recovery path");
    runManualRefresh();
    return;
  }
  if (simulatorAlertPollPending) {
    simulatorAlertPollPending = false;
    LOG_INF("STICK", "Simulator invoking one Project.Stick alert poll");
    PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll();
    return;
  }
#endif

  // X3 side buttons are fixed physical controls: BTN_UP is on the left edge
  // and BTN_DOWN is on the right edge.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(false, false);
      service.refreshScheduledContent();
      showFeedbackBubble(FeedbackBubble::Meh);
      requestCloudSync(false);
      setStatus(tr(STR_PROJECT_STICK_MEH_SENT));
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (service.display().copyId != 0) {
      service.sendFeedback(true, false);
      showFeedbackBubble(FeedbackBubble::Useful);
      requestCloudSync(false);
      setStatus(tr(STR_PROJECT_STICK_USEFUL_SENT));
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
    runManualRefresh();
    return;
  }

  const uint32_t nowMs = millis();
  if (state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastManifestAttemptMs >= service.pollIntervalSeconds() * 1000UL) {
    const bool heartbeatDue = project_stick::registrationDue(nowMs, lastRegisterMs);
    if (requestCloudSync(heartbeatDue)) lastManifestAttemptMs = nowMs;
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
    if (PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll()) lastAlertPollMs = nowMs;
  }
}

void ProjectStickActivity::launchWifiSelection() {
  startActivityForResult(
      std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
      [this](const ActivityResult&) {
        if (WiFi.status() == WL_CONNECTED) {
          state = State::Connecting;
          setStatus(tr(STR_PROJECT_STICK_SYNCING));
          requestCloudSync(true);
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

  const auto display = service.displaySnapshot();
  const int hintTop = height - metrics.buttonHintsHeight;
  const int contentInset =
      std::max(metrics.contentSidePadding, SIDE_BUTTON_MARGIN + SIDE_BUTTON_WIDTH + SIDE_CONTENT_GAP);
  const int syncLineHeight = synchronizedAtLine[0] == '\0'
                                 ? 0
                                 : renderer.getTextLineHeight(UI_10_FONT_ID, synchronizedAtLine);
  const int syncLineY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (syncLineHeight > 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, syncLineY, synchronizedAtLine);
  }
  const int contentTop = syncLineY + syncLineHeight +
                         (syncLineHeight > 0 ? metrics.verticalSpacing : 0);
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
  drawFeedbackBubble(width);
  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_PROJECT_STICK_CONNECT_WIFI), "",
                      tr(STR_PROJECT_STICK_REFRESH));
  renderer.displayBuffer();
}

void ProjectStickActivity::setStatus(const char* text) {
  RenderLock lock;
  snprintf(statusLine, sizeof(statusLine), "%s", text ? text : "");
}

void ProjectStickActivity::recordSynchronizedAt(const project_stick::ShanghaiTime& time) {
  if (!time.valid) return;
  const unsigned hour = time.secondOfDay / 3600;
  const unsigned minute = (time.secondOfDay / 60) % 60;
  RenderLock lock;
  snprintf(synchronizedAtLine, sizeof(synchronizedAtLine), tr(STR_PROJECT_STICK_SYNCED_AT), hour,
           minute);
}

void ProjectStickActivity::showFeedbackBubble(FeedbackBubble bubble) {
  {
    RenderLock lock;
    feedbackBubble = bubble;
    feedbackBubbleStartedMs = millis();
    feedbackBubbleFrame = 0;
  }
  requestUpdate(true);
}

void ProjectStickActivity::updateFeedbackBubble() {
  bool repaint = false;
  {
    RenderLock lock;
    if (feedbackBubble == FeedbackBubble::None) return;
    const uint32_t elapsed = millis() - feedbackBubbleStartedMs;
    if (elapsed >= FEEDBACK_BUBBLE_VISIBLE_MS) {
      feedbackBubble = FeedbackBubble::None;
      repaint = true;
    } else {
      const uint8_t nextFrame =
          std::min<uint8_t>(FEEDBACK_BUBBLE_FINAL_FRAME, elapsed / FEEDBACK_BUBBLE_FRAME_MS);
      if (nextFrame != feedbackBubbleFrame) {
        feedbackBubbleFrame = nextFrame;
        repaint = true;
      }
    }
  }
  if (repaint) requestUpdate();
}

void ProjectStickActivity::drawFeedbackBubble(int screenWidth) const {
  if (feedbackBubble == FeedbackBubble::None) return;
  const char* text = feedbackBubble == FeedbackBubble::Meh
                         ? tr(STR_PROJECT_STICK_MEH_BUBBLE)
                         : tr(STR_PROJECT_STICK_USEFUL_BUBBLE);
  constexpr int horizontalPadding = 18;
  constexpr int verticalPadding = 11;
  constexpr int sideGap = 8;
  constexpr int borderWidth = 2;
  constexpr int cornerRadius = 10;
  const int textWidth = renderer.getTextWidth(NOTOSANSSC_13_FONT_ID, text);
  const int textHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, text);
  const int maxWidth = screenWidth - 2 * (SIDE_BUTTON_MARGIN + SIDE_BUTTON_WIDTH + sideGap);
  const int bubbleWidth = std::min(maxWidth, textWidth + horizontalPadding * 2);
  const int bubbleHeight = textHeight + verticalPadding * 2;
  const int finalX = (screenWidth - bubbleWidth) / 2;
  const bool fromLeft = feedbackBubble == FeedbackBubble::Meh;
  const int startX = fromLeft ? SIDE_BUTTON_MARGIN + SIDE_BUTTON_WIDTH + sideGap
                              : screenWidth - SIDE_BUTTON_MARGIN - SIDE_BUTTON_WIDTH - sideGap -
                                    bubbleWidth;
  const int x = startX + (finalX - startX) * feedbackBubbleFrame /
                             FEEDBACK_BUBBLE_FINAL_FRAME;
  const int y = SIDE_BUTTON_Y + (SIDE_BUTTON_HEIGHT - bubbleHeight) / 2;
  renderer.fillRoundedRect(x, y, bubbleWidth, bubbleHeight, cornerRadius, Color::White);
  renderer.drawRoundedRect(x, y, bubbleWidth, bubbleHeight, borderWidth, cornerRadius, true);
  const Rect bounds{x, y, bubbleWidth, bubbleHeight};
  UITheme::drawCenteredText(renderer, bounds, NOTOSANSSC_13_FONT_ID, y + verticalPadding, text,
                            true, EpdFontFamily::BOLD);
}
