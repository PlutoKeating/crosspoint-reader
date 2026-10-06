#pragma once

#include <ProjectStickNetPolicy.h>
#include <ProjectStickWifiPolicy.h>

#include <cstdint>
#include <string>

#include "network/WifiAutoConnect.h"
#include "project_stick/ProjectStickService.h"

// Everything the StockStick product does that must not depend on which page
// is open: applying what a phone asks for over BLE (bind, sync, unbind, Wi-Fi
// join and scan, OTA), keeping the STATE characteristic fresh, bringing Wi-Fi
// back, the cloud heartbeat and alert polls, and the clock. Ticked from the
// main loop on the UI task, next to studio_ble::tick(), so a phone gets the
// same answers whether the device shows a card, Settings or the home menu.
// The StockStick page only renders: it reads the state below.
class ProjectStickHost {
 public:
  static ProjectStickHost& getInstance();

  // Once from setup(), after storage and settings are loaded. Until then
  // tick() does nothing (SD card error, early boot).
  void begin();
  void tick(uint32_t nowMs);

  // The UI-side service (clock, binding, alert display). UI task only.
  ProjectStickService& service() { return service_; }

  // --- State the pages render from (UI task; setupPayload under RenderLock) ---
  // Changes whenever something a status screen shows did: binding, the setup
  // QR, activation, an alert, ownership.
  uint32_t generation() const { return generation_; }
  // Deactivated in the console (register answered 403).
  bool inactive() const { return inactive_; }
  // Setup QR payload of an unbound device, empty otherwise.
  const std::string& setupPayload() const { return setupPayload_; }
  bool wifiJoinActive() const { return bleWifiActive_; }
  bool wifiScanActive() const { return bleScanActive_; }
  // The latest one-shot outcome, for transient notices: a page shows it when
  // `sequence` moved since it last looked.
  enum class Event : uint8_t { None, WifiConnected, WifiFailed, Synced, SyncFailed };
  struct EventMark {
    Event event = Event::None;
    uint32_t sequence = 0;
  };
  EventMark lastEvent() const { return event_; }

  // The cloud heartbeat; skipped while a phone sync is fresh unless `manual`
  // (the user closed the Wi-Fi screen, which is an explicit "sync now"). The
  // job waits for the on-demand Wi-Fi link itself.
  bool requestCloudSync(bool manual = false);
  void wifiRetrySoon() { wifiAutoConnect_.retrySoon(); }
  // The Wi-Fi selection screen drives the radio itself while it is open.
  void suspendWifiAutoConnect(bool suspended);
  // Wi-Fi is on demand (2.7.2, lib/ProjectStick/ProjectStickWifiPolicy.h):
  // pages that need the link hold it while they are open.
  void holdWifi(bool hold);
  // At least one network is saved: the device can join on demand (the status
  // tag shows standby rather than offline while the radio is down).
  bool wifiNetworkSaved() const {
#ifdef SIMULATOR
    return true;  // the simulator's fake network needs no credentials
#else
    return wifiSaved_;
#endif
  }

  // True while page-independent work is running that needs the CPU at full
  // speed (no idle power saving): a phone link, a transfer, a Wi-Fi join or
  // scan asked over BLE, a cloud job, a firmware update.
  bool busy() const;

 private:
  ProjectStickHost() = default;
  ProjectStickHost(const ProjectStickHost&) = delete;
  ProjectStickHost& operator=(const ProjectStickHost&) = delete;

  static constexpr uint32_t BLE_WIFI_TIMEOUT_MS = 20000;
  static constexpr uint32_t BLE_SCAN_TIMEOUT_MS = 15000;
  // STATE characteristic refresh: often while a phone is connected, rarely otherwise.
  static constexpr uint32_t STATE_REFRESH_LINKED_MS = 2000;
  static constexpr uint32_t STATE_REFRESH_IDLE_MS = 10UL * 60UL * 1000UL;
  // An unbound device re-checks its setup key this often (allocates a string).
  static constexpr uint32_t SETUP_CHECK_MS = 500;
  static constexpr uint32_t POWER_RECHECK_MS = 30000;
  static constexpr uint32_t TAKEOVER_RETRY_MS = 2000;

  void touch() { ++generation_; }
  void post(Event event) { event_ = {event, event_.sequence + 1}; }
  void tickPower(uint32_t nowMs);
  void tickBle(uint32_t nowMs);
  void tickOnline(uint32_t nowMs);
  void tickTakeover(uint32_t nowMs);
  void applyBackgroundResult();
  // Keeps STATUS `wifi.state` equal to the real link (a saved network joined
  // by itself counts as much as one the phone pushed).
  void mirrorWifiState();
  void startBleWifi(const std::string& ssid, const std::string& password);
  void pollBleWifi();
  // A linked, active phone holds background cloud jobs back (project_stick::phoneLinkAction).
  bool backgroundJobDeferred() const;
  project_stick::WifiNeeds wifiNeeds(uint32_t nowMs) const;
  void powerOffWifi(uint32_t nowMs);
  void refreshWifiSaved();
  void startBleScan();
  void pollBleScan();
  // Publishes STATE; `nowMs == 0` forces a refresh.
  void refreshBleState(uint32_t nowMs);

  bool begun_ = false;
  ProjectStickService service_;
  WifiAutoConnect wifiAutoConnect_;
  bool wifiSuspended_ = false;
  uint32_t generation_ = 0;
  EventMark event_;
  bool inactive_ = false;
  std::string setupPayload_;
  uint32_t lastSetupCheckMs_ = 0;
  uint32_t backgroundResultSequence_ = 0;

  std::string bleWifiSsid_;
  bool bleWifiActive_ = false;
  bool bleScanActive_ = false;
  bool bleScanPending_ = false;
  bool wifiReportedUp_ = false;
  bool wifiSaved_ = false;
  bool wifiPageHold_ = false;
  uint32_t wifiJoinHoldUntilMs_ = 0;  // keep the link after a BLE join so the phone reads `connected`
  uint32_t lastWifiWantedMs_ = 0;
  bool wifiWantedLogged_ = true;
  uint32_t lastWifiSavedCheckMs_ = 0;
  uint32_t bleWifiStartedMs_ = 0;
  uint32_t bleScanStartedMs_ = 0;
  // A cloud sync owed once the phone lets go of the link (bind, came online).
  bool syncAfterBle_ = false;
  // An OTA the phone requested over BLE, held until the worker accepts it.
  bool otaPending_ = false;
  ProjectStickService::FirmwareTarget otaTarget_;
  project_stick::Deadline otaQueueDeadline_;

  uint32_t lastSyncAttemptMs_ = 0;
  uint32_t lastAlertPollMs_ = 0;
  uint32_t lastRegisterMs_ = 0;
  uint32_t lastStateRefreshMs_ = 0;
  bool stateLinkWasConnected_ = false;
  uint32_t lastPowerCheckMs_ = 0;
  uint32_t lastTakeoverMs_ = 0;
#ifdef SIMULATOR
  bool simulatorAlertPollPending_ = false;
#endif
};

#define PROJECT_STICK_HOST ProjectStickHost::getInstance()
