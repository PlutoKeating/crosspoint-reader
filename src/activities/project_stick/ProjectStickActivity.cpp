#include "ProjectStickActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "ProjectStickCore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "project_stick/FirmwareUpdateState.h"
#include "project_stick/ProjectStickBackgroundSync.h"
#include "project_stick/StudioBluetooth.h"
#include "project_stick/StudioFrame.h"
#include "util/QrUtils.h"

namespace {
constexpr int NETWORK_TAG_BOTTOM_GAP = 12;
constexpr int NETWORK_TAG_HORIZONTAL_PADDING = 8;
constexpr int NETWORK_TAG_DOT_SIZE = 5;
constexpr int NETWORK_TAG_DOT_GAP = 5;
constexpr int COMPACT_HEADER_BATTERY_RESERVE = 90;
constexpr int QR_SIZE = 185;  // whole 5 px modules for version 5 (37 modules)
constexpr int QR_GAP = 18;
constexpr int PROMPT_GAP = 12;

constexpr int STATUS_TAG_GAP = 8;

// One outline pill in the header: a dot (filled when `active`) and a label.
// Tags stack leftwards from `rightInset`; returns the inset for the next one.
int drawStatusTag(const GfxRenderer& renderer, const Rect& headerBounds, int rightInset, const char* label,
                  bool connected) {
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
  return safeRightInset + tagWidth + STATUS_TAG_GAP;
}

const char* bluetoothTagLabel(const studio_ble::Radio radio) {
  if (radio == studio_ble::Radio::Connected) return tr(STR_PROJECT_STICK_BT_CONNECTED);
  if (radio == studio_ble::Radio::Advertising) return tr(STR_PROJECT_STICK_BT_READY);
  return tr(STR_PROJECT_STICK_BT_OFF);
}

int64_t epochSeconds(const project_stick::ShanghaiTime& time) {
  return time.valid ? time.day * 86400LL + time.secondOfDay - 8 * 3600 : 0;
}
}  // namespace

void ProjectStickActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Portrait);
  // Normally started in setup(); recovery mode skips it until this page opens.
  PROJECT_STICK_HOST.begin();
  const auto link = studio_ble::link();
  seenFailedTransfers = link.failed;
  lastRadio = link.radio;
  hostGeneration = PROJECT_STICK_HOST.generation();
  hostEventSequence = PROJECT_STICK_HOST.lastEvent().sequence;
  PROJECT_STICK_HOST.requestCloudSync();
  requestUpdate();
}

void ProjectStickActivity::updateButtonHints(const uint32_t nowMs) {
  // Hints appear on any key action and hide after 5 s; the 20 s keyguard
  // always engages later, and locked presses show the unlock prompt instead.
  // Unlocking shows them right away (the unlock release itself is consumed,
  // so it would not count as a key action) and starts the same 5 s countdown.
  const bool locked = mappedInput.isKeyguardLocked();
  const bool justUnlocked = keyguardWasLocked && !locked;
  keyguardWasLocked = locked;
  if (locked) {
    buttonHintsVisible = false;
    return;
  }
  if (justUnlocked || mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
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
  // Checked before taking the render lock: with no bubble up (almost always)
  // this must not wait for an e-paper refresh — that wait stalled the BLE
  // chunk queue during every repaint of a transfer.
  if (feedbackBubble == stick_overlay::Bubble::None) return;
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

void ProjectStickActivity::showTransientNotice(const stick_overlay::Notice result, const uint32_t durationMs) {
  transientNotice = result;
  transientNoticeUntilMs = millis() + durationMs;
}

// Picks the one notice to show and repaints the moment it changes; busy
// notices also advance their animation (and transfer progress) once per frame.
void ProjectStickActivity::updateNotice(const uint32_t nowMs) {
  using stick_overlay::Notice;
  const auto link = studio_ble::link();
  if (link.failed != seenFailedTransfers) {
    seenFailedTransfers = link.failed;
    showTransientNotice(Notice::Failed, NOTICE_FAILURE_MS);
  } else if (lastTransfer == studio_ble::Transfer::Refreshing && link.transfer == studio_ble::Transfer::None) {
    showTransientNotice(Notice::Done, NOTICE_RESULT_MS);
  }
  lastTransfer = link.transfer;
  // One-shot outcomes of work ProjectStickHost ran (a Wi-Fi join asked over
  // BLE, the cloud heartbeat this page showed as "syncing").
  const auto hostEvent = PROJECT_STICK_HOST.lastEvent();
  if (hostEvent.sequence != hostEventSequence) {
    hostEventSequence = hostEvent.sequence;
    using Event = ProjectStickHost::Event;
    if (hostEvent.event == Event::WifiConnected) {
      showTransientNotice(Notice::WifiConnected, NOTICE_RESULT_MS);
    } else if (hostEvent.event == Event::WifiFailed) {
      showTransientNotice(Notice::WifiFailed, NOTICE_FAILURE_MS);
    } else if (syncNoticeShown && (hostEvent.event == Event::Synced || hostEvent.event == Event::SyncFailed)) {
      syncNoticeShown = false;
      if (hostEvent.event == Event::Synced)
        showTransientNotice(Notice::Synced, NOTICE_RESULT_MS);
      else
        showTransientNotice(Notice::SyncFailed, NOTICE_FAILURE_MS);
    }
  }
  if (transientNotice != Notice::None && static_cast<int32_t>(nowMs - transientNoticeUntilMs) >= 0)
    transientNotice = Notice::None;

  Notice next = Notice::None;
  int percent = 0;
  if (link.transfer == studio_ble::Transfer::Receiving) {
    next = Notice::Receiving;
    percent = link.total ? static_cast<int>(std::min<uint64_t>(100, uint64_t(link.received) * 100 / link.total)) : 0;
  } else if (link.transfer == studio_ble::Transfer::Refreshing) {
    next = Notice::Refreshing;
  } else if (link.resuming) {
    next = Notice::Resuming;
  } else if (PROJECT_STICK_HOST.wifiJoinActive()) {
    next = Notice::WifiConnecting;
  } else if (PROJECT_STICK_HOST.wifiScanActive()) {
    next = Notice::WifiScanning;
  } else if (transientNotice != Notice::None) {
    next = transientNotice;
  } else if (link.radio == studio_ble::Radio::Connected) {
    next = Notice::PhoneConnected;
  } else if (PROJECT_STICK_BACKGROUND_SYNC.runningKind() == ProjectStickBackgroundSync::WorkKind::Sync) {
    next = Notice::Syncing;
    syncNoticeShown = true;
  }

  // The status screen also carries the radio state in its header tag.
  const bool radioChanged = link.radio != lastRadio;
  lastRadio = link.radio;
  const bool kindChanged = next != notice;
  // A repaint re-reads the card from SD and refreshes the panel; while a
  // transfer streams to the same card it animates more slowly.
  const uint32_t frameMs = next == Notice::Receiving ? RECEIVING_FRAME_MS : stick_overlay::NOTICE_FRAME_MS;
  const bool frameDue = stick_overlay::noticeBusy(next) && nowMs - noticeFrameMs >= frameMs;
  if (!kindChanged && !frameDue) {
    if (radioChanged && !StudioFrame::instance().hasContent()) requestUpdate();
    return;
  }
  {
    RenderLock lock;
    noticeFrame = kindChanged ? 0 : noticeFrame + 1;
    notice = next;
    noticePercent = percent;
  }
  noticeFrameMs = nowMs;
  requestUpdate(kindChanged);
}

void ProjectStickActivity::loop() {
  updateButtonHints(millis());
  updateFeedbackBubble();
  updateNotice(millis());
  // Binding, the setup QR, activation, alerts and the online state change in
  // ProjectStickHost; repaint when any of them did.
  if (hostGeneration != PROJECT_STICK_HOST.generation()) {
    hostGeneration = PROJECT_STICK_HOST.generation();
    requestUpdate();
  }
  auto& frame = StudioFrame::instance();
  // No snapshot() here: it copies heap strings on every loop iteration.
  const bool hasStudio = frame.hasContent();
  const int64_t studioNow = epochSeconds(service.now());
  const int64_t alertUntil = epochSeconds(service.displaySnapshot().alertUntil);
  frame.tick(studioNow, 0, alertUntil);
  const auto firmwareUpdate = firmware_update::snapshot();
  if (firmwareUpdate.generation != firmwareUpdateGeneration) {
    firmwareUpdateGeneration = firmwareUpdate.generation;
    requestUpdate();  // progress, or restore the content once the update stops
  }
  if (studioGeneration != frame.generation()) {
    studioGeneration = frame.generation();
    requestUpdate();
  }

  // X3 side buttons are fixed physical controls: BTN_UP is on the left edge
  // ("一般": next card) and BTN_DOWN is on the right edge ("有用": keep).
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (hasStudio) {
      const auto studio = frame.snapshot();
      service.sendStudioFeedback(studio.task, studio.card, false);
      frame.tick(studioNow, 1, alertUntil);
      showFeedbackBubble(stick_overlay::Bubble::Meh);
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (hasStudio) {
      frame.tick(studioNow, 2, alertUntil);
      const auto visual = frame.displaySnapshot();
      service.sendStudioFeedback(visual.task, visual.card, true);
      showFeedbackBubble(stick_overlay::Bubble::Useful);
    }
    return;
  }

  // Fixed physical front-button positions: Back / Wi-Fi / unassigned /
  // "换一张" (only while a Studio card is shown).
  const int frontButton = mappedInput.getPressedFrontButton();
  if (frontButton == HalGPIO::BTN_BACK) {
    onGoHome(HomeMenuItem::PROJECT_STICK);
    return;
  }
  if (frontButton == HalGPIO::BTN_CONFIRM) {
    launchWifiSelection();
    return;
  }
  if (frontButton == HalGPIO::BTN_RIGHT && hasStudio) {
    // The program plays on the device: the right key is the manual "next card".
    frame.tick(studioNow, 1, alertUntil);
    requestUpdate();
    return;
  }
}

void ProjectStickActivity::launchWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                         [this](const ActivityResult&) {
                           PROJECT_STICK_HOST.wifiRetrySoon();
                           PROJECT_STICK_HOST.requestCloudSync(true);
                           requestUpdate();
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

// Shown until a Studio frame is installed: the BLE setup QR while unbound,
// otherwise a prompt to deliver content from the mini program over BLE.
void ProjectStickActivity::renderStatusScreen() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const bool online = WiFi.status() == WL_CONNECTED;
  renderer.clearScreen();
  const Rect headerBounds{0, metrics.topPadding, width, metrics.headerHeight};
  GUI.drawHeader(renderer, headerBounds, tr(STR_PROJECT_STICK));
  // Wi-Fi is on demand (2.7.2): a saved network with the radio idle is the
  // normal state, not a fault.
  const char* wifiLabel = online                                 ? tr(STR_PROJECT_STICK_STATUS_ONLINE)
                          : PROJECT_STICK_HOST.wifiNetworkSaved() ? tr(STR_PROJECT_STICK_STATUS_STANDBY)
                                                                  : tr(STR_PROJECT_STICK_STATUS_OFFLINE);
  const int bluetoothInset =
      drawStatusTag(renderer, headerBounds, metrics.contentSidePadding, wifiLabel, online);
  const auto radio = studio_ble::link().radio;
  drawStatusTag(renderer, headerBounds, bluetoothInset, bluetoothTagLabel(radio),
                radio == studio_ble::Radio::Connected || radio == studio_ble::Radio::Advertising);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int hintTop = height - metrics.buttonHintsHeight;
  const Rect content{metrics.contentSidePadding, contentTop, width - metrics.contentSidePadding * 2,
                     hintTop - metrics.verticalSpacing - contentTop};

  if (PROJECT_STICK_HOST.inactive()) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_PROJECT_STICK_INACTIVE), 4, true,
                                     EpdFontFamily::BOLD);
  } else if (!service.isBound()) {
    // BLE setup (protocol 3): the QR carries the device id and the one-time
    // key; the phone binds and pushes Wi-Fi over Bluetooth.
    const std::string& setupPayload = PROJECT_STICK_HOST.setupPayload();
    const char* title = tr(STR_PROJECT_STICK_SETUP_TITLE);
    const char* helper = tr(STR_PROJECT_STICK_SETUP_HELP);
    const int titleHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, title);
    const int helperLine = renderer.getTextLineHeight(NOTOSANSSC_10_FONT_ID, helper);
    const bool cjk = std::any_of(helper, helper + strlen(helper), [](char c) { return c & 0x80; });
    const auto helperLines = cjk ? renderer.wrappedCjkText(NOTOSANSSC_10_FONT_ID, helper, content.width - 40, 3)
                                 : renderer.wrappedText(NOTOSANSSC_10_FONT_ID, helper, content.width - 40, 3);
    const int helperHeight = helperLine * static_cast<int>(helperLines.size());
    const int qrSize = setupPayload.empty() ? 0 : QR_SIZE;
    const int groupHeight = titleHeight + QR_GAP + qrSize + QR_GAP + helperHeight;
    const int groupY = content.y + (content.height - groupHeight) / 2;
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY, title, true, EpdFontFamily::BOLD);
    const int qrY = groupY + titleHeight + QR_GAP;
    if (qrSize > 0) QrUtils::drawQrCode(renderer, Rect{(width - QR_SIZE) / 2, qrY, QR_SIZE, QR_SIZE}, setupPayload);
    int y = qrY + qrSize + QR_GAP;
    for (const auto& line : helperLines) {
      UITheme::drawCenteredText(renderer, content, NOTOSANSSC_10_FONT_ID, y, line.c_str(), true);
      y += helperLine;
    }
  } else {
    const char* title = tr(STR_PROJECT_STICK_NO_LOCAL_CONTENT);
    const char* helper = tr(STR_PROJECT_STICK_AWAIT_CONTENT);
    const int titleHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, title);
    const int helperLine = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, helper);
    const bool cjk = std::any_of(helper, helper + strlen(helper), [](char c) { return c & 0x80; });
    const auto helperLines = cjk ? renderer.wrappedCjkText(NOTOSANSSC_13_FONT_ID, helper, content.width - 40, 3)
                                 : renderer.wrappedText(NOTOSANSSC_13_FONT_ID, helper, content.width - 40, 3);
    const int helperHeight = helperLine * static_cast<int>(helperLines.size());
    const int groupY = content.y + (content.height - titleHeight - PROMPT_GAP - helperHeight) / 2;
    UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, groupY, title, true, EpdFontFamily::BOLD);
    int y = groupY + titleHeight + PROMPT_GAP;
    for (const auto& line : helperLines) {
      UITheme::drawCenteredText(renderer, content, NOTOSANSSC_13_FONT_ID, y, line.c_str(), true);
      y += helperLine;
    }
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
    if (studio) stick_overlay::drawCardSideKeyHints(renderer, feedbackBubble == stick_overlay::Bubble::None);
    stick_overlay::drawFrontKeyHints(renderer, tr(STR_PROJECT_STICK_BACK), tr(STR_PROJECT_STICK_CONNECT_WIFI), "",
                                     studio ? tr(STR_PROJECT_STICK_NEXT_CARD) : "");
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
    stick_overlay::drawNotice(renderer, notice, noticePercent, noticeFrame);
    renderer.displayBuffer();
    // Overlays are transient; the card underneath is what was displayed.
    StudioFrame::instance().displayed();
    return;
  }
  // An installed program with no current frame keeps the last visual; only
  // the lock can be layered onto it.
  if (StudioFrame::instance().hasContent()) {
    if (mappedInput.isKeyguardLocked()) {
      drawOverlays(false);
      renderer.displayBuffer();
    }
    return;
  }
  renderStatusScreen();
  drawOverlays(false);
  stick_overlay::drawNotice(renderer, notice, noticePercent, noticeFrame);
  renderer.displayBuffer();
}


// Full speed while a phone is linked, a program is being installed, or the
// installed content is not a portable (self-contained) program yet.
bool ProjectStickActivity::needsFullPower() {
  return !StudioFrame::instance().portable() || studio_ble::connected() || StudioFrame::instance().busy();
}
bool ProjectStickActivity::skipLoopDelay() { return needsFullPower(); }
