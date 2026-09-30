#include "ProjectStickActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "ProjectStickCore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "project_stick/FirmwareUpdateState.h"
#include "project_stick/StudioBluetooth.h"
#include "project_stick/StudioFrame.h"
#include "util/QrUtils.h"

namespace {
constexpr int NETWORK_TAG_BOTTOM_GAP = 12;
constexpr int NETWORK_TAG_HORIZONTAL_PADDING = 8;
constexpr int NETWORK_TAG_DOT_SIZE = 5;
constexpr int NETWORK_TAG_DOT_GAP = 5;
constexpr int COMPACT_HEADER_BATTERY_RESERVE = 90;
constexpr int QR_SIZE = 180;
constexpr int QR_GAP = 18;
constexpr int CODE_GAP = 24;
constexpr int PROMPT_GAP = 12;
constexpr uint32_t STUDIO_POLL_MS = 5000;

void drawNetworkStatusTag(const GfxRenderer& renderer, const Rect& headerBounds, int rightInset, bool connected) {
  const char* label = connected ? tr(STR_PROJECT_STICK_STATUS_ONLINE) : tr(STR_PROJECT_STICK_STATUS_OFFLINE);
  // Chinese labels resolve to the Noto Sans SC 12 fallback; size the tag to
  // the font that actually renders so the glyphs stay inside the outline.
  const int textHeight = renderer.getTextLineHeight(SMALL_FONT_ID, label);
  const int tagHeight = textHeight + 4;
  // The pill's ends are half-circles; keep content clear of the curve.
  const int horizontalPadding = NETWORK_TAG_HORIZONTAL_PADDING + tagHeight / 4;
  const int tagWidth = horizontalPadding * 2 + NETWORK_TAG_DOT_SIZE + NETWORK_TAG_DOT_GAP +
                       renderer.getTextWidth(SMALL_FONT_ID, label);
  const bool hasSecondHeaderRow = headerBounds.height >= 60;
  const int safeRightInset = hasSecondHeaderRow ? rightInset : std::max(rightInset, COMPACT_HEADER_BATTERY_RESERVE);
  const int tagX = renderer.getScreenWidth() - safeRightInset - tagWidth;
  const int tagY = hasSecondHeaderRow ? headerBounds.y + headerBounds.height - tagHeight - NETWORK_TAG_BOTTOM_GAP
                                      : headerBounds.y + (headerBounds.height - tagHeight) / 2;
  const int dotX = tagX + horizontalPadding;
  const int dotY = tagY + (tagHeight - NETWORK_TAG_DOT_SIZE) / 2;

  renderer.drawRoundedRect(tagX, tagY, tagWidth, tagHeight, 1, tagHeight / 2, true);
  if (connected) {
    renderer.fillRect(dotX, dotY, NETWORK_TAG_DOT_SIZE, NETWORK_TAG_DOT_SIZE);
  } else {
    renderer.drawRect(dotX, dotY, NETWORK_TAG_DOT_SIZE, NETWORK_TAG_DOT_SIZE);
  }
  renderer.drawText(SMALL_FONT_ID, dotX + NETWORK_TAG_DOT_SIZE + NETWORK_TAG_DOT_GAP, tagY + 2, label);
}

int64_t epochSeconds(const project_stick::ShanghaiTime& time) {
  return time.valid ? time.day * 86400LL + time.secondOfDay - 8 * 3600 : 0;
}
}  // namespace

void ProjectStickActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Portrait);
  service.begin();
  PROJECT_STICK_BACKGROUND_SYNC.begin();
  backgroundResultSequence = PROJECT_STICK_BACKGROUND_SYNC.latestSequence();
  if (WiFi.status() == WL_CONNECTED) {
    state = State::Connecting;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    requestCloudSync(true);
  } else {
    state = State::Offline;
    setStatus(tr(STR_PROJECT_STICK_OFFLINE));
  }
  requestUpdate();
}

bool ProjectStickActivity::requestCloudSync(bool registerFirst) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const bool queued = PROJECT_STICK_BACKGROUND_SYNC.requestSync(registerFirst);
  if (queued) lastSyncAttemptMs = millis();
  return queued;
}

// Front-right "同步": register (refreshes binding, owner and clock), then poll
// the Studio target in the same burst.
void ProjectStickActivity::syncNow() {
  if (WiFi.status() != WL_CONNECTED) {
    state = State::Offline;
    setStatus(tr(STR_PROJECT_STICK_OFFLINE));
    requestUpdate();
    return;
  }
  state = State::Connecting;
  setStatus(tr(STR_PROJECT_STICK_SYNCING));
  requestCloudSync(true);
  requestUpdate();
}

void ProjectStickActivity::applyBackgroundResult() {
  ProjectStickBackgroundSync::Result result;
  if (!PROJECT_STICK_BACKGROUND_SYNC.takeResult(backgroundResultSequence, result)) return;

  if (result.kind == ProjectStickBackgroundSync::WorkKind::AlertPoll) {
    if (result.alertReceived) {
      service.adoptDisplay(std::move(result.alertDisplay));
      requestUpdate();
    }
    return;
  }
  if (result.kind != ProjectStickBackgroundSync::WorkKind::Sync) return;

  const auto& report = result.syncReport;
  service.adoptServerTime(report.synchronizedAt);
  if (project_stick::shouldRecordRegisterSuccess(report)) lastRegisterMs = millis();
  lastAlertPollMs = millis();
  updateState(report);
#ifdef SIMULATOR
  simulatorAlertPollPending = report.result == ProjectStickService::SyncResult::Synced &&
                              std::getenv("CROSSPOINT_SIM_POLL_ALERT_ONCE") != nullptr;
#endif
}

void ProjectStickActivity::updateState(const project_stick::SyncReport& report) {
  switch (report.result) {
    case ProjectStickService::SyncResult::Synced:
    case ProjectStickService::SyncResult::Unbound:
      state = State::Online;
      setStatus(tr(STR_PROJECT_STICK_ONLINE));
      break;
    case ProjectStickService::SyncResult::Inactive:
      state = State::Inactive;
      setStatus(tr(STR_PROJECT_STICK_INACTIVE));
      break;
    case ProjectStickService::SyncResult::Failed:
      state = WiFi.status() == WL_CONNECTED ? State::Error : State::Offline;
      setStatus(state == State::Error ? tr(STR_PROJECT_STICK_FAILED) : tr(STR_PROJECT_STICK_OFFLINE));
      break;
  }
  requestUpdate();
}

void ProjectStickActivity::updateButtonHints(const uint32_t nowMs) {
  // Hints appear on any key action and hide after 5 s; the 20 s keyguard
  // always engages later, and locked presses show the unlock prompt instead.
  if (mappedInput.isKeyguardLocked()) {
    buttonHintsVisible = false;
    return;
  }
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
    lastKeyActionMs = nowMs;
    if (!buttonHintsVisible) {
      buttonHintsVisible = true;
      requestUpdate();
    }
    return;
  }
  if (buttonHintsVisible && nowMs - lastKeyActionMs >= BUTTON_HINT_TIMEOUT_MS) {
    buttonHintsVisible = false;
    requestUpdate();
  }
}

void ProjectStickActivity::showFeedbackBubble(const stick_overlay::Bubble bubble) {
  {
    RenderLock lock;
    feedbackBubble = bubble;
    feedbackBubbleStartedMs = millis();
    feedbackBubbleFrame = 0;
  }
  requestUpdate(true);
}

// Advances the bubble through its discrete slide frames, then clears it.
void ProjectStickActivity::updateFeedbackBubble() {
  bool repaint = false;
  {
    RenderLock lock;
    if (feedbackBubble == stick_overlay::Bubble::None) return;
    const uint32_t elapsed = millis() - feedbackBubbleStartedMs;
    if (elapsed >= stick_overlay::BUBBLE_VISIBLE_MS || mappedInput.isKeyguardLocked()) {
      feedbackBubble = stick_overlay::Bubble::None;
      repaint = true;
    } else {
      const uint8_t frame =
          std::min<uint32_t>(stick_overlay::BUBBLE_FINAL_FRAME, elapsed / stick_overlay::BUBBLE_FRAME_MS);
      if (frame != feedbackBubbleFrame) {
        feedbackBubbleFrame = frame;
        repaint = true;
      }
    }
  }
  if (repaint) requestUpdate();
}

void ProjectStickActivity::loop() {
  updateButtonHints(millis());
  updateFeedbackBubble();
  studio_ble::tick();
  if (service.refreshOwnership()) requestUpdate();
  auto& frame = StudioFrame::instance();
  const auto studio = frame.snapshot();
  const int64_t studioNow = epochSeconds(service.now());
  const int64_t alertUntil = epochSeconds(service.displaySnapshot().alertUntil);
  frame.tick(studioNow, 0, alertUntil);
  if (!firmware_update::snapshot().busy() && wifiAutoConnect.tick(millis()) && state != State::Inactive) {
    // Came online on its own (boot, wake, OTA restart or a recovered link).
    state = State::Connecting;
    setStatus(tr(STR_PROJECT_STICK_SYNCING));
    requestCloudSync(true);
    lastStudioPollMs = millis() - STUDIO_POLL_MS;  // poll the Studio target right away
    requestUpdate();
  }
  const auto firmwareUpdate = firmware_update::snapshot();
  if (firmwareUpdate.generation != firmwareUpdateGeneration) {
    firmwareUpdateGeneration = firmwareUpdate.generation;
    requestUpdate();  // progress, or restore the content once the update stops
  }
  if (studioGeneration != frame.generation()) {
    studioGeneration = frame.generation();
    requestUpdate();
  }
  if (!studio_ble::connected() && WiFi.status() == WL_CONNECTED && millis() - lastStudioPollMs >= STUDIO_POLL_MS) {
    if (PROJECT_STICK_BACKGROUND_SYNC.requestStudioPoll()) lastStudioPollMs = millis();
  }
  applyBackgroundResult();
#ifdef SIMULATOR
  if (simulatorAlertPollPending) {
    simulatorAlertPollPending = false;
    LOG_INF("STICK", "Simulator invoking one Project.Stick alert poll");
    PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll();
    return;
  }
#endif

  // X3 side buttons are fixed physical controls: BTN_UP is on the left edge
  // ("一般": next card) and BTN_DOWN is on the right edge ("有用": keep).
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (!studio.hash.empty()) {
      service.sendStudioFeedback(studio.task, studio.card, false);
      frame.tick(studioNow, 1, alertUntil);
      showFeedbackBubble(stick_overlay::Bubble::Meh);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (!studio.hash.empty()) {
      frame.tick(studioNow, 2, alertUntil);
      service.sendStudioFeedback(frame.displaySnapshot().task, frame.displaySnapshot().card, true);
      showFeedbackBubble(stick_overlay::Bubble::Useful);
    }
    return;
  }

  // Fixed physical front-button positions: Back / Wi-Fi / unassigned / Sync
  // (or "换一张" while a Studio card is shown).
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
    if (!studio.hash.empty()) {
      // With a Studio program installed the right key is the manual "next card".
      frame.tick(studioNow, 1, alertUntil);
      requestUpdate();
      requestCloudSync(project_stick::registrationDue(millis(), lastRegisterMs));
      return;
    }
    syncNow();
    return;
  }

  // Periodic register heartbeat keeps binding, owner and clock current; the
  // Studio target itself is polled every few seconds above.
  const uint32_t nowMs = millis();
  if (!studio_ble::connected() && state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastSyncAttemptMs >= service.pollIntervalSeconds() * 1000UL) {
    requestCloudSync(project_stick::registrationDue(nowMs, lastRegisterMs) || !service.isBound());
  }
  if (!studio_ble::connected() && state != State::Inactive && WiFi.status() == WL_CONNECTED &&
      nowMs - lastAlertPollMs >= service.alertPollIntervalSeconds() * 1000UL) {
    if (PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll()) lastAlertPollMs = nowMs;
  }
}

void ProjectStickActivity::launchWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                         [this](const ActivityResult&) {
                           wifiAutoConnect.retrySoon();
                           if (WiFi.status() == WL_CONNECTED) {
                             state = State::Connecting;
                             setStatus(tr(STR_PROJECT_STICK_SYNCING));
                             requestCloudSync(true);
                           } else {
                             state = State::Offline;
                             setStatus(tr(STR_PROJECT_STICK_OFFLINE));
                           }
                         });
}

void ProjectStickActivity::renderFirmwareUpdate() {
  const auto update = firmware_update::snapshot();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const char* title = tr(STR_OTA_DOWNLOADING);
  if (update.phase == firmware_update::Phase::Verifying) title = tr(STR_OTA_VERIFYING);
  if (update.phase == firmware_update::Phase::Installing) title = tr(STR_OTA_INSTALLING);
  if (update.phase == firmware_update::Phase::Restarting) title = tr(STR_OTA_RESTARTING);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_OTA_TITLE));
  int y = height / 2 - lineHeight * 2;
  renderer.drawCenteredText(UI_12_FONT_ID, y, title, true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y, update.version);
  y += lineHeight + metrics.verticalSpacing;
  if (update.phase != firmware_update::Phase::Restarting) {
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, y, width - metrics.contentSidePadding * 2,
                             metrics.progressBarHeight},
                        update.percent(), 100);
  }
  renderer.displayBuffer();
}

// Shown until a Studio frame is installed: the binding code while unbound,
// otherwise a prompt to publish the official plan or a card.
void ProjectStickActivity::renderStatusScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const bool online = WiFi.status() == WL_CONNECTED;
  renderer.clearScreen();
  const Rect headerBounds{0, metrics.topPadding, width, metrics.headerHeight};
  GUI.drawHeader(renderer, headerBounds, tr(STR_PROJECT_STICK));
  drawNetworkStatusTag(renderer, headerBounds, metrics.contentSidePadding, online);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int hintTop = height - metrics.buttonHintsHeight;
  const Rect content{metrics.contentSidePadding, contentTop, width - metrics.contentSidePadding * 2,
                     hintTop - metrics.verticalSpacing - contentTop};

  if (state == State::Inactive || state == State::Error) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, statusLine, 4, true, EpdFontFamily::BOLD);
  } else if (!service.isBound()) {
    const std::string code = service.pairingCode();
    const char* title = tr(STR_PROJECT_STICK_BIND_TITLE);
    char codeLine[48];
    if (code.empty()) {
      snprintf(codeLine, sizeof(codeLine), "%s",
               online ? tr(STR_PROJECT_STICK_BIND_LOADING) : tr(STR_PROJECT_STICK_CONNECT_TO_SYNC));
    } else {
      snprintf(codeLine, sizeof(codeLine), "%s  %s", tr(STR_PROJECT_STICK_BIND_CODE), code.c_str());
    }
    const int titleHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, title);
    const int codeHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, codeLine);
    const int qrSize = code.empty() ? 0 : QR_SIZE;
    const int qrGap = qrSize > 0 ? QR_GAP : 0;
    const int groupHeight = titleHeight + qrGap + qrSize + CODE_GAP + codeHeight;
    const int groupY = content.y + (content.height - groupHeight) / 2;
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY, title, true, EpdFontFamily::BOLD);
    if (qrSize > 0) {
      const Rect qrBounds{(width - qrSize) / 2, groupY + titleHeight + qrGap, qrSize, qrSize};
      QrUtils::drawQrCode(renderer, qrBounds, "stockstick://bind?code=" + code);
    }
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY + titleHeight + qrGap + qrSize + CODE_GAP,
                              codeLine, true, EpdFontFamily::BOLD);
  } else {
    const char* title = tr(STR_PROJECT_STICK_NO_LOCAL_CONTENT);
    const char* helper = online ? tr(STR_PROJECT_STICK_AWAIT_CONTENT) : tr(STR_PROJECT_STICK_CONNECT_TO_SYNC);
    const int titleHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, title);
    const int helperHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, helper);
    const int groupY = content.y + (content.height - titleHeight - PROMPT_GAP - helperHeight) / 2;
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY, title, true, EpdFontFamily::BOLD);
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY + titleHeight + PROMPT_GAP, helper,
                              true);
  }

}

// Transient layers over the frame or status screen: the unlock prompt while
// locked, otherwise the key hints and the feedback window.
void ProjectStickActivity::drawOverlays(const bool studio) {
  if (mappedInput.isKeyguardLocked()) {
    stick_overlay::drawKeyguard(renderer, mappedInput.keyguardState(), mappedInput.isKeyguardPromptVisible(),
                                mappedInput.keyguardCueFrame());
    return;
  }
  if (buttonHintsVisible) {
    if (studio) stick_overlay::drawSideKeyHints(renderer, feedbackBubble == stick_overlay::Bubble::None);
    stick_overlay::drawFrontKeyHints(renderer, tr(STR_PROJECT_STICK_BACK), tr(STR_PROJECT_STICK_CONNECT_WIFI), "",
                                     studio ? tr(STR_PROJECT_STICK_NEXT_CARD) : tr(STR_PROJECT_STICK_SYNC));
  }
  if (studio) stick_overlay::drawFeedbackBubble(renderer, feedbackBubble, feedbackBubbleFrame);
}

void ProjectStickActivity::render(RenderLock&&) {
  // A firmware update owns the screen: the device restarts when it finishes.
  if (firmware_update::snapshot().busy()) {
    renderFirmwareUpdate();
    return;
  }
  if (StudioFrame::instance().render(renderer)) {
    drawOverlays(true);
    renderer.displayBuffer();
    // Overlays are transient; the card underneath is what was displayed.
    StudioFrame::instance().displayed();
    return;
  }
  // An installed program with no current frame keeps the last visual; only
  // the lock can be layered onto it.
  if (!StudioFrame::instance().snapshot().hash.empty()) {
    if (mappedInput.isKeyguardLocked()) {
      drawOverlays(false);
      renderer.displayBuffer();
    }
    return;
  }
  renderStatusScreen();
  drawOverlays(false);
  renderer.displayBuffer();
}

void ProjectStickActivity::setStatus(const char* text) {
  RenderLock lock;
  snprintf(statusLine, sizeof(statusLine), "%s", text ? text : "");
}

bool ProjectStickActivity::allowIdlePowerSaving() {
  return StudioFrame::instance().portable() && !studio_ble::connected() && !StudioFrame::instance().busy();
}
bool ProjectStickActivity::skipLoopDelay() { return !allowIdlePowerSaving(); }
