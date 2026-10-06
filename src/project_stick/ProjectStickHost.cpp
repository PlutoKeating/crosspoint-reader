#include "ProjectStickHost.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdlib>
#include <utility>
#include <vector>

#include "ProjectStickCore.h"
#include "ProjectStickHostPolicy.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/RenderLock.h"
#include "project_stick/FirmwareUpdateState.h"
#include "project_stick/ProjectStickBackgroundSync.h"
#include "project_stick/StudioBluetooth.h"
#include "project_stick/StudioFrame.h"

ProjectStickHost& ProjectStickHost::getInstance() {
  static ProjectStickHost instance;
  return instance;
}

void ProjectStickHost::begin() {
  if (begun_) return;
  service_.begin();
  // Bring the radio up before Wi-Fi and TLS take their share of the heap; an
  // unbound device starts it from tickBle() once it has a setup key.
  studio_ble::begin();
  backgroundResultSequence_ = PROJECT_STICK_BACKGROUND_SYNC.latestSequence();
  firmware_update::setExternalPower(gpio.isUsbConnected());
  begun_ = true;
}

bool ProjectStickHost::busy() const {
  return begun_ && (studio_ble::connected() || StudioFrame::instance().busy() || bleWifiActive_ || bleScanActive_ ||
                    bleScanPending_ || otaPending_ || firmware_update::snapshot().busy() ||
                    PROJECT_STICK_BACKGROUND_SYNC.busy());
}

void ProjectStickHost::suspendWifiAutoConnect(const bool suspended) {
  wifiSuspended_ = suspended;
  // Pick the link up (or retry the saved networks) as soon as the screen closes.
  if (!suspended) wifiAutoConnect_.retrySoon();
}

// Register heartbeat (plus events and an OTA outcome) of a bound device.
// Content never comes from the cloud; unbound devices make no device API
// requests. Since 2.6.0 the phone relays the same data over BLE (STATE + op
// `sync`), so the heartbeat only runs when no phone has synced within the
// heartbeat interval.
bool ProjectStickHost::requestCloudSync(const bool manual) {
  if (!begun_ || WiFi.status() != WL_CONNECTED || ProjectStickService::apiBlocked() || !service_.hasCredential())
    return false;
  if (!manual && service_.phoneSyncFresh()) return false;
  const bool queued = PROJECT_STICK_BACKGROUND_SYNC.requestSync();
  if (queued) lastSyncAttemptMs_ = millis();
  return queued;
}

void ProjectStickHost::tick(const uint32_t nowMs) {
  if (!begun_) return;
  tickPower(nowMs);
  service_.syncClock();
  tickBle(nowMs);
  refreshBleState(nowMs);
  if (service_.refreshOwnership()) touch();
  tickOnline(nowMs);
  applyBackgroundResult();
  tickTakeover(nowMs);
}

// The install guard on the worker needs to know whether external power is
// charging the battery; the fuel gauge sits on the UI task's I2C bus.
void ProjectStickHost::tickPower(const uint32_t nowMs) {
  if (!gpio.wasUsbStateChanged() && nowMs - lastPowerCheckMs_ < POWER_RECHECK_MS) return;
  lastPowerCheckMs_ = nowMs;
  firmware_update::setExternalPower(gpio.isUsbConnected());
}

// Applies work a phone queued over BLE (studio_ble callbacks never touch the
// SD card or the radio themselves).
void ProjectStickHost::tickBle(const uint32_t nowMs) {
  // Setup mode: an unbound device needs a one-time key to be discoverable and
  // a QR to show, on whichever page it sits.
  if (lastSetupCheckMs_ == 0 || nowMs - lastSetupCheckMs_ >= SETUP_CHECK_MS) {
    lastSetupCheckMs_ = nowMs ? nowMs : 1;
    std::string payload;
    if (!service_.isBound()) {
      payload = studio_ble::setupPayload();
      if (payload.empty()) {
        studio_ble::setup(service_.deviceId());
        payload = studio_ble::setupPayload();
      }
    }
    if (payload != setupPayload_) {
      {
        RenderLock lock;
        setupPayload_ = std::move(payload);
      }
      touch();
    }
  }

  studio_ble::Binding binding;
  if (studio_ble::takeBinding(binding)) {
    RenderLock lock;
    const bool ok = service_.applyBleBinding(binding.token, binding.owner);
    studio_ble::finishBinding(ok, binding);
    if (ok) {
      inactive_ = false;
      // Register with the new token once the phone lets go of the link.
      syncAfterBle_ = true;
      setupPayload_.clear();
    }
    lock.unlock();
    touch();
  }

  studio_ble::SyncRequest sync;
  if (studio_ble::takeSyncRequest(sync)) {
    {
      RenderLock lock;
      service_.applyPhoneSync(sync);
    }
    // The phone relayed the heartbeat: restart the cloud interval from here.
    lastRegisterMs_ = millis();
    refreshBleState(0);
    touch();
  }
  if (studio_ble::takeUnbindRequest()) {
    {
      RenderLock lock;
      service_.unbindFromPhone();
    }
    lastSetupCheckMs_ = 0;  // back to setup mode on the next tick
    touch();
  }

  studio_ble::WifiRequest wifi;
  if (studio_ble::takeWifiRequest(wifi)) startBleWifi(wifi.ssid, wifi.password);
  if (bleWifiActive_) pollBleWifi();
  mirrorWifiState();
  if (studio_ble::takeScanRequest()) bleScanPending_ = true;
  if (bleScanPending_ && !bleWifiActive_ && !bleScanActive_) startBleScan();
  if (bleScanActive_) pollBleScan();

  studio_ble::OtaRequest ota;
  if (studio_ble::takeOtaRequest(ota)) {
    otaTarget_ = {ota.version, ota.url, ota.sha256, ota.bytes};
    otaPending_ = true;
    otaQueueDeadline_.start(millis(), project_stick::OTA_QUEUE_DEADLINE_MS);
  }
  if (otaPending_ && PROJECT_STICK_BACKGROUND_SYNC.requestFirmwareInstall(otaTarget_)) {
    otaPending_ = false;
    otaQueueDeadline_.stop();
  } else if (otaPending_ && otaQueueDeadline_.expired(millis())) {
    // The worker never became free: report it to the phone (STATUS.ota) rather
    // than leaving the request queued forever.
    LOG_ERR("OTA", "BLE update request not accepted by the worker in time");
    otaPending_ = false;
    otaQueueDeadline_.stop();
    firmware_update::fail("device_busy");
  }
}

void ProjectStickHost::startBleWifi(const std::string& ssid, const std::string& password) {
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
  wifiAutoConnect_.holdOff(millis(), BLE_WIFI_TIMEOUT_MS + 5000);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (password.empty())
    WiFi.begin(ssid.c_str());
  else
    WiFi.begin(ssid.c_str(), password.c_str());
  bleWifiSsid_ = ssid;
  bleWifiActive_ = true;
  bleWifiStartedMs_ = millis();
  studio_ble::reportWifi(studio_ble::WifiState::Connecting, ssid);
}

void ProjectStickHost::mirrorWifiState() {
  if (bleWifiActive_) return;  // a join the phone asked for reports its own progress
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up == wifiReportedUp_) return;
  wifiReportedUp_ = up;
  if (up)
    studio_ble::reportWifi(studio_ble::WifiState::Connected, WiFi.SSID().c_str());
  else
    studio_ble::reportWifi(studio_ble::WifiState::Idle, "");
}

void ProjectStickHost::pollBleWifi() {
  const wl_status_t status = WiFi.status();
  const uint32_t elapsed = millis() - bleWifiStartedMs_;
  const char* error = nullptr;
  if (status == WL_CONNECTED) {
    bleWifiActive_ = false;
    studio_ble::reportWifi(studio_ble::WifiState::Connected, bleWifiSsid_);
    wifiReportedUp_ = true;
    post(Event::WifiConnected);
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
  bleWifiActive_ = false;
  WiFi.disconnect();
  studio_ble::reportWifi(studio_ble::WifiState::Failed, bleWifiSsid_, error);
  wifiReportedUp_ = false;  // the failure stays readable until a link comes up
  post(Event::WifiFailed);
  wifiAutoConnect_.retrySoon();
}

void ProjectStickHost::startBleScan() {
  bleScanPending_ = false;
  wifiAutoConnect_.holdOff(millis(), BLE_SCAN_TIMEOUT_MS + 5000);
  WiFi.mode(WIFI_STA);
  // A half-finished join makes the driver refuse to scan.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  WiFi.scanNetworks(true);
  bleScanActive_ = true;
  bleScanStartedMs_ = millis();
  studio_ble::reportScan(true);
}

void ProjectStickHost::pollBleScan() {
  const int result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING && millis() - bleScanStartedMs_ < BLE_SCAN_TIMEOUT_MS) return;
  std::vector<ble_setup::Network> found;
  for (int i = 0; i < result; ++i)
    found.push_back({WiFi.SSID(i).c_str(), static_cast<int>(WiFi.RSSI(i)), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  WiFi.scanDelete();
  bleScanActive_ = false;
  studio_ble::reportScan(false, std::move(found));
  if (WiFi.status() != WL_CONNECTED) wifiAutoConnect_.retrySoon();
}

// Publishes the STATE characteristic (firmware, metrics, pending events, OTA
// outcome) the phone reads during a BLE session.
void ProjectStickHost::refreshBleState(uint32_t nowMs) {
  // Nothing competes with a running transfer for the main loop and the card.
  if (nowMs != 0 && StudioFrame::instance().busy()) return;
  const bool connected = studio_ble::connected();
  const bool justConnected = connected && !stateLinkWasConnected_;  // fresh values before the phone's first read
  stateLinkWasConnected_ = connected;
  const uint32_t interval = connected ? STATE_REFRESH_LINKED_MS : STATE_REFRESH_IDLE_MS;
  if (nowMs != 0 && !justConnected && lastStateRefreshMs_ != 0 && nowMs - lastStateRefreshMs_ < interval) return;
  lastStateRefreshMs_ = nowMs ? nowMs : millis();
  studio_ble::setState(service_.phoneStateJson());
}

// Wi-Fi comes back by itself on every page, and the cloud link follows it:
// the register heartbeat and, in trading hours, the alert poll.
void ProjectStickHost::tickOnline(const uint32_t nowMs) {
  const bool updating = firmware_update::snapshot().busy();
  if (!updating && !wifiSuspended_ && wifiAutoConnect_.tick(nowMs) && !inactive_) {
    // Came online on its own (boot, wake, OTA restart, a recovered link or a
    // BLE Wi-Fi push). Cloud requests wait while a phone is connected over
    // BLE: a setup session may be binding the device right now.
    if (studio_ble::connected())
      syncAfterBle_ = true;
    else
      requestCloudSync();
    touch();
  }
  if (syncAfterBle_ && !studio_ble::connected()) {
    syncAfterBle_ = false;
    if (WiFi.status() == WL_CONNECTED && !inactive_) requestCloudSync();
    touch();
  }
#ifdef SIMULATOR
  if (simulatorAlertPollPending_) {
    simulatorAlertPollPending_ = false;
    LOG_INF("STICK", "Simulator invoking one Project.Stick alert poll");
    PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll();
    return;
  }
#endif
  // Register heartbeat every 6 h (binding, owner, trading day, clock), retried
  // no faster than poll_interval_seconds. Nothing goes out while the shared
  // API backoff holds (429 / 5xx / unreachable) or without a credential.
  const bool cloudAllowed = !updating && !studio_ble::connected() && !inactive_ && WiFi.status() == WL_CONNECTED &&
                            !ProjectStickService::apiBlocked() && service_.hasCredential();
  if (cloudAllowed && project_stick::registrationDue(nowMs, lastRegisterMs_) &&
      nowMs - lastSyncAttemptMs_ >= service_.pollIntervalSeconds() * 1000UL) {
    requestCloudSync();
  }
  if (cloudAllowed && service_.inAlertWindow() &&
      nowMs - lastAlertPollMs_ >= service_.alertPollIntervalSeconds() * 1000UL) {
    if (PROJECT_STICK_BACKGROUND_SYNC.requestAlertPoll()) lastAlertPollMs_ = nowMs;
  }
}

void ProjectStickHost::applyBackgroundResult() {
  ProjectStickBackgroundSync::Result result;
  if (!PROJECT_STICK_BACKGROUND_SYNC.takeResult(backgroundResultSequence_, result)) return;

  if (result.kind == ProjectStickBackgroundSync::WorkKind::AlertPoll) {
    if (result.alertReceived) {
      service_.adoptDisplay(std::move(result.alertDisplay));
      touch();
    }
    return;
  }
  if (result.kind != ProjectStickBackgroundSync::WorkKind::Sync) return;

  const auto& report = result.syncReport;
  if (report.registerSucceeded)
    post(Event::Synced);
  else if (report.registerAttempted)
    post(Event::SyncFailed);
  service_.adoptServerTime(report.synchronizedAt);
  if (project_stick::shouldRecordRegisterSuccess(report)) lastRegisterMs_ = millis();
  lastAlertPollMs_ = millis();
  // A failed heartbeat changes nothing on screen: content plays locally and
  // the shared backoff schedules the next attempt. Only a deactivated device
  // says so.
  const bool deactivated = report.result == ProjectStickService::SyncResult::Inactive;
  if (report.result != ProjectStickService::SyncResult::Failed && deactivated != inactive_) {
    inactive_ = deactivated;
    touch();
  }
#ifdef SIMULATOR
  simulatorAlertPollPending_ = report.result == ProjectStickService::SyncResult::Synced &&
                               std::getenv("CROSSPOINT_SIM_POLL_ALERT_ONCE") != nullptr;
#endif
}

// What a phone starts must be visible wherever the device sits: content it
// delivers is shown (and its "displayed" receipt produced) by the StockStick
// page, and a firmware update owns the screen until the restart.
void ProjectStickHost::tickTakeover(const uint32_t nowMs) {
  const auto page = activityManager.stickTakeover();
  if (page == StickTakeover::StickPage || page == StickTakeover::Never) return;
  if (lastTakeoverMs_ != 0 && nowMs - lastTakeoverMs_ < TAKEOVER_RETRY_MS) return;
  const bool updating = firmware_update::snapshot().busy();
  const bool content = studio_ble::link().transfer != studio_ble::Transfer::None;
  if (!project_stick::shouldShowStickPage(page, updating, content)) return;
  lastTakeoverMs_ = nowMs ? nowMs : 1;
  LOG_INF("STICK", "Showing the StockStick page for %s", updating ? "a firmware update" : "incoming content");
  activityManager.goToProjectStick();
}
