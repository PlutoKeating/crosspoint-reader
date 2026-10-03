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
#include "WifiCredentialStore.h"
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
  service.begin();
  // Bring the radio up before Wi-Fi and TLS take their share of the heap; an
  // unbound device starts it from tickBleSetup() once it has a setup key.
  studio_ble::begin();
  const auto link = studio_ble::link();
  seenFailedTransfers = link.failed;
  lastRadio = link.radio;
  backgroundResultSequence = PROJECT_STICK_BACKGROUND_SYNC.latestSequence();
  requestCloudSync();
  requestUpdate();
}

// Register heartbeat (plus events and an OTA outcome) of a bound device.
// Content never comes from the cloud; unbound devices make no requests. Since
// 2.6.0 the phone relays the same data over BLE (STATE + op `sync`), so the
// heartbeat only runs when no phone has synced within the heartbeat interval.
bool ProjectStickActivity::requestCloudSync(bool manual) {
  if (WiFi.status() != WL_CONNECTED || ProjectStickService::apiBlocked() || !service.hasCredential()) return false;
  if (!manual && service.phoneSyncFresh()) return false;
  const bool queued = PROJECT_STICK_BACKGROUND_SYNC.requestSync();
  if (queued) lastSyncAttemptMs = millis();
  return queued;
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
  if (syncNoticeShown) {
    syncNoticeShown = false;
    if (report.registerSucceeded)
      showTransientNotice(stick_overlay::Notice::Synced, NOTICE_RESULT_MS);
    else if (report.registerAttempted)
      showTransientNotice(stick_overlay::Notice::SyncFailed, NOTICE_FAILURE_MS);
  }
  service.adoptServerTime(report.synchronizedAt);
  if (project_stick::shouldRecordRegisterSuccess(report)) lastRegisterMs = millis();
  lastAlertPollMs = millis();
  updateState(report);
#ifdef SIMULATOR
  simulatorAlertPollPending = report.result == ProjectStickService::SyncResult::Synced &&
                              std::getenv("CROSSPOINT_SIM_POLL_ALERT_ONCE") != nullptr;
#endif
}

// A failed heartbeat changes nothing on screen: content plays locally and the
// shared backoff schedules the next attempt. Only a deactivated device says so.
void ProjectStickActivity::updateState(const project_stick::SyncReport& report) {
  const bool deactivated = report.result == ProjectStickService::SyncResult::Inactive;
  if (report.result == ProjectStickService::SyncResult::Failed || deactivated == inactive) return;
  inactive = deactivated;
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
  } else if (bleWifiActive) {
    next = Notice::WifiConnecting;
  } else if (bleScanActive) {
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
  service.syncClock();
  tickBleSetup();
  refreshBleState(millis());
  updateNotice(millis());
  if (service.refreshOwnership()) requestUpdate();
  auto& frame = StudioFrame::instance();
  // No snapshot() here: it copies heap strings on every loop iteration.
  const bool hasStudio = frame.hasContent();
  const int64_t studioNow = epochSeconds(service.now());
  const int64_t alertUntil = epochSeconds(service.displaySnapshot().alertUntil);
  frame.tick(studioNow, 0, alertUntil);
  auto startOnlineSync = [this] {
    requestCloudSync();
    requestUpdate();
  };
  if (!firmware_update::snapshot().busy() && wifiAutoConnect.tick(millis()) && !inactive) {
    // Came online on its own (boot, wake, OTA restart, a recovered link or a
    // BLE Wi-Fi push). Cloud requests wait while a phone is connected over
    // BLE: a setup session may be binding the device right now.
    if (studio_ble::connected())
      syncAfterBle = true;
    else
      startOnlineSync();
  }
  if (syncAfterBle && !studio_ble::connected()) {
    syncAfterBle = false;
    if (WiFi.status() == WL_CONNECTED && !inactive) startOnlineSync();
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
  if (otaPending && PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareInstall(otaTarget)) {
    otaPending = false;
    otaQueueDeadline.stop();
  } else if (otaPending && otaQueueDeadline.expired(millis())) {
    // The worker never became free: report it to the phone (STATUS.ota) rather
    // than leaving the request queued forever.
    LOG_ERR("OTA", "BLE update request not accepted by the worker in time");
    otaPending = false;
    otaQueueDeadline.stop();
    firmware_update::fail("device_busy");
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

  // Register heartbeat every 6 h (binding, owner, trading day, clock), retried
  // no faster than poll_interval_seconds. Nothing goes out while the shared
  // API backoff holds (429 / 5xx / unreachable) or without a credential.
  const uint32_t nowMs = millis();
  const bool cloudAllowed = !studio_ble::connected() && !inactive && WiFi.status() == WL_CONNECTED &&
                            !ProjectStickService::apiBlocked() && service.hasCredential();
  if (cloudAllowed && project_stick::registrationDue(nowMs, lastRegisterMs) &&
      nowMs - lastSyncAttemptMs >= service.pollIntervalSeconds() * 1000UL) {
    requestCloudSync();
  }
  if (cloudAllowed && service.inAlertWindow() && nowMs - lastAlertPollMs >= service.alertPollIntervalSeconds() * 1000UL) {
    if (PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll()) lastAlertPollMs = nowMs;
  }
}

// Applies work a phone queued over BLE (studio_ble callbacks never touch the
// SD card or the radio themselves).
void ProjectStickActivity::tickBleSetup() {
  if (!service.isBound()) studio_ble::setup(service.deviceId());
  const std::string payload = service.isBound() ? std::string() : studio_ble::setupPayload();
  if (payload != setupPayload) {
    {
      RenderLock lock;
      setupPayload = payload;
    }
    requestUpdate();
  }

  studio_ble::Binding binding;
  if (studio_ble::takeBinding(binding)) {
    RenderLock lock;
    const bool ok = service.applyBleBinding(binding.token, binding.owner);
    studio_ble::finishBinding(ok, binding);
    if (ok) {
      inactive = false;
      // Register with the new token once the phone lets go of the link.
      syncAfterBle = true;
    }
    lock.unlock();
    requestUpdate();
  }

  studio_ble::SyncRequest sync;
  if (studio_ble::takeSyncRequest(sync)) {
    RenderLock lock;
    service.applyPhoneSync(sync);
    // The phone relayed the heartbeat: restart the cloud interval from here.
    lastRegisterMs = millis();
    lock.unlock();
    refreshBleState(0);
    requestUpdate();
  }
  if (studio_ble::takeUnbindRequest()) {
    RenderLock lock;
    service.unbindFromPhone();
    lock.unlock();
    requestUpdate();
  }

  studio_ble::WifiRequest wifi;
  if (studio_ble::takeWifiRequest(wifi)) startBleWifi(wifi.ssid, wifi.password);
  if (bleWifiActive) pollBleWifi();
  if (studio_ble::takeScanRequest()) bleScanPending = true;
  studio_ble::OtaRequest ota;
  if (studio_ble::takeOtaRequest(ota)) {
    otaTarget = {ota.version, ota.url, ota.sha256, ota.bytes};
    otaPending = true;
    otaQueueDeadline.start(millis(), project_stick::OTA_QUEUE_DEADLINE_MS);
  }
  if (bleScanPending && !bleWifiActive && !bleScanActive) startBleScan();
  if (bleScanActive) pollBleScan();
}

void ProjectStickActivity::startBleWifi(const std::string& ssid, const std::string& password) {
  {
    RenderLock lock;
    // Disk is the source of truth; the auto-connect helper may not have
    // loaded the list yet and saving an empty in-memory list would drop it.
    WIFI_STORE.loadFromFile();
    if (!WIFI_STORE.addCredential(ssid, password)) {
      // The store is full: forget the oldest network other than the current one.
      for (const auto& saved : WIFI_STORE.getCredentials()) {
        if (saved.ssid == WIFI_STORE.getLastConnectedSsid()) continue;
        const std::string victim = saved.ssid;
        WIFI_STORE.removeCredential(victim);
        break;
      }
      WIFI_STORE.addCredential(ssid, password);
    }
    WIFI_STORE.setLastConnectedSsid(ssid);
  }
  wifiAutoConnect.holdOff(millis(), BLE_WIFI_TIMEOUT_MS + 5000);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (password.empty())
    WiFi.begin(ssid.c_str());
  else
    WiFi.begin(ssid.c_str(), password.c_str());
  bleWifiSsid = ssid;
  bleWifiActive = true;
  bleWifiStartedMs = millis();
  studio_ble::reportWifi(studio_ble::WifiState::Connecting, ssid);
}

void ProjectStickActivity::pollBleWifi() {
  const wl_status_t status = WiFi.status();
  const uint32_t elapsed = millis() - bleWifiStartedMs;
  const char* error = nullptr;
  if (status == WL_CONNECTED) {
    bleWifiActive = false;
    studio_ble::reportWifi(studio_ble::WifiState::Connected, bleWifiSsid);
    showTransientNotice(stick_overlay::Notice::WifiConnected, NOTICE_RESULT_MS);
    return;
  }
  // The driver reports these once the join has had time to scan and handshake.
  if (elapsed >= 3000 && status == WL_NO_SSID_AVAIL)
    error = "no_ap";
  else if (elapsed >= 3000 && status == WL_CONNECT_FAILED)
    error = "wrong_password";
  else if (elapsed >= BLE_WIFI_TIMEOUT_MS)
    error = "timeout";
  if (!error) return;
  bleWifiActive = false;
  WiFi.disconnect();
  studio_ble::reportWifi(studio_ble::WifiState::Failed, bleWifiSsid, error);
  showTransientNotice(stick_overlay::Notice::WifiFailed, NOTICE_FAILURE_MS);
  wifiAutoConnect.retrySoon();
}

void ProjectStickActivity::startBleScan() {
  bleScanPending = false;
  wifiAutoConnect.holdOff(millis(), BLE_SCAN_TIMEOUT_MS + 5000);
  WiFi.mode(WIFI_STA);
  // A half-finished join makes the driver refuse to scan.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  WiFi.scanNetworks(true);
  bleScanActive = true;
  bleScanStartedMs = millis();
  studio_ble::reportScan(true);
}

void ProjectStickActivity::pollBleScan() {
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING && millis() - bleScanStartedMs < BLE_SCAN_TIMEOUT_MS) return;
  std::vector<ble_setup::Network> found;
  for (int i = 0; i < result; ++i)
    found.push_back({WiFi.SSID(i).c_str(), static_cast<int>(WiFi.RSSI(i)), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  WiFi.scanDelete();
  bleScanActive = false;
  studio_ble::reportScan(false, std::move(found));
  if (WiFi.status() != WL_CONNECTED) wifiAutoConnect.retrySoon();
}

// Publishes the STATE characteristic (firmware, metrics, pending events, OTA
// outcome) the phone reads during a BLE session. `nowMs == 0` forces a refresh.
void ProjectStickActivity::refreshBleState(uint32_t nowMs) {
  // Nothing competes with a running transfer for the main loop and the card.
  if (nowMs != 0 && StudioFrame::instance().busy()) return;
  const bool connected = studio_ble::connected();
  const bool justConnected = connected && !stateLinkWasConnected;  // fresh values before the phone's first read
  stateLinkWasConnected = connected;
  const uint32_t interval = connected ? STATE_REFRESH_LINKED_MS : STATE_REFRESH_IDLE_MS;
  if (nowMs != 0 && !justConnected && lastStateRefreshMs != 0 && nowMs - lastStateRefreshMs < interval) return;
  lastStateRefreshMs = nowMs ? nowMs : millis();
  studio_ble::setState(service.phoneStateJson());
}

void ProjectStickActivity::launchWifiSelection() {
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false),
                         [this](const ActivityResult&) {
                           wifiAutoConnect.retrySoon();
                           requestCloudSync(true);
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
  const int bluetoothInset =
      drawStatusTag(renderer, headerBounds, metrics.contentSidePadding,
                    online ? tr(STR_PROJECT_STICK_STATUS_ONLINE) : tr(STR_PROJECT_STICK_STATUS_OFFLINE), online);
  const auto radio = studio_ble::link().radio;
  drawStatusTag(renderer, headerBounds, bluetoothInset, bluetoothTagLabel(radio),
                radio == studio_ble::Radio::Connected || radio == studio_ble::Radio::Advertising);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int hintTop = height - metrics.buttonHintsHeight;
  const Rect content{metrics.contentSidePadding, contentTop, width - metrics.contentSidePadding * 2,
                     hintTop - metrics.verticalSpacing - contentTop};

  if (inactive) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_PROJECT_STICK_INACTIVE), 4, true,
                                     EpdFontFamily::BOLD);
  } else if (!service.isBound()) {
    // BLE setup (protocol 3): the QR carries the device id and the one-time
    // key; the phone binds and pushes Wi-Fi over Bluetooth.
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


bool ProjectStickActivity::allowIdlePowerSaving() {
  return StudioFrame::instance().portable() && !studio_ble::connected() && !StudioFrame::instance().busy();
}
bool ProjectStickActivity::skipLoopDelay() { return !allowIdlePowerSaving(); }
