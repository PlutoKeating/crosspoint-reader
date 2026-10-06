#pragma once

#include <ProjectStickCore.h>
#include <ProjectStickNetPolicy.h>
#include <ProjectStickSyncState.h>
#include <SecureHttpClient.h>

#include "StudioBluetooth.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The device's remaining cloud link (docs/studio-protocol.md "Cloud
// requests"). Content arrives over BLE only; the cloud is used by bound
// devices for the register heartbeat, trading-hours alerts and events. A
// device without a device token (unbound, or the website simulator) makes no
// device API requests. The firmware catalogue and image downloads are public
// and need only Wi-Fi (Settings > Firmware update works unbound).
class ProjectStickService {
 public:
  using SyncResult = project_stick::SyncResult;
  using SyncReport = project_stick::SyncReport;

  // Market alert state handed from the background worker to the UI. The
  // Studio program shows its alert scene while alertUntil lies in the future.
  struct Display {
    project_stick::ShanghaiTime alertUntil;
  };

  void begin();
  // Register heartbeat, then pending events and an unreported OTA outcome.
  SyncReport sync();
  bool pollAlerts();

  // A firmware image to install: from the catalogue (Settings > Firmware
  // update) or from the phone over BLE (op `ota`).
  struct FirmwareTarget {
    std::string version, url, sha256;
    size_t bytes = 0;
  };
  struct FirmwareOffer {
    enum class Status : uint8_t { UpdateAvailable, UpToDate, Failed, InstallFailed };
    Status status = Status::Failed;
    // Why a check failed (shown on the firmware screen); httpStatus for Server.
    project_stick::NetFailure failure = project_stick::NetFailure::None;
    int httpStatus = 0;
    FirmwareTarget target;
    std::string notes;
  };
  // Asks the public catalogue (no binding, no credential: Wi-Fi is enough)
  // and compares the published version with the running one.
  FirmwareOffer checkFirmware();
  // Downloads (resumable), verifies and flashes `target`, then restarts. Only
  // returns on failure; progress and errors go to firmware_update.
  void installFirmware(const FirmwareTarget& target);
  void sendStudioFeedback(const std::string& task, const std::string& card, bool useful);
  bool refreshOwnership();

  // BLE-first sync (2.6.0, docs/studio-protocol.md "Phone-relayed sync"): the
  // phone reads STATE, uploads it and answers with op `sync`. UI loop only
  // (store writes, SD usage, OTA outcome in NVS).
  std::string phoneStateJson();
  void applyPhoneSync(const studio_ble::SyncRequest& request);
  // Op `unbind`: the owner unbound the device in the mini program next to it.
  void unbindFromPhone();
  // True while the last phone sync is younger than the heartbeat interval: the
  // cloud heartbeat (register + events) is skipped, Wi-Fi stays for alerts.
  bool phoneSyncFresh() const;

  const Display& display() const { return currentDisplay; }
  Display displaySnapshot() const;
  void adoptDisplay(Display display);
  void adoptServerTime(const project_stick::ShanghaiTime& time);
  uint32_t pendingEventCount() const;
  uint32_t pollIntervalSeconds() const;
  uint32_t alertPollIntervalSeconds() const;
  // Shared cloud backoff (429 / 5xx / unreachable): no request goes out while
  // blocked. A manual firmware check may clear an error backoff but never a
  // server rate limit (429).
  static bool apiBlocked();
  static bool clearBackoffForManualSync();
  bool isTradingDay() const;
  // Trading day and inside A-share continuous trading hours (alerts can only change then).
  bool inAlertWindow() const;
  bool hasClock() const { return serverTime.valid; }
  bool isBound() const;
  // True when the device holds a cloud credential (a bound physical device).
  bool hasCredential() const;
  std::string deviceId() const;
  // Stores the credential a phone delivered over BLE setup (protocol 3):
  // device token, owner and bound=true. A registration already in flight
  // with the previous identity state is discarded instead of applied.
  bool applyBleBinding(const std::string& deviceToken, const std::string& owner);
  project_stick::ShanghaiTime now() const;
  // Copies trusted time between the system clock and the RTC (UI task only).
  void syncClock();

 private:
  std::string baseUrl, localOwner;
  void adoptOwner(const std::string& owner);
  // The server no longer accepts this device's credential (unbound in the
  // mini program, or claimed again): back to BLE setup mode.
  void dropBinding();
  Display currentDisplay;
  project_stick::ShanghaiTime serverTime;
  uint32_t serverTimeCapturedMs = 0;
  static constexpr uint32_t CLOCK_SYNC_MS = 60000;
  uint32_t lastClockSyncMs = 0;
  bool inactive = false;
  freeink::SecureHttpClient http;

  bool ensureIdentity();
  bool registerDevice(int& status);
  enum class DownloadResult : uint8_t { Complete, Retry, Fatal };
  bool validFirmwareTarget(const FirmwareTarget& target) const;
  DownloadResult downloadFirmware(const FirmwareTarget& target);
  bool requestPost(const std::string& path, const std::string& body, std::string& response, int& status);
  bool fetchJson(const std::string& url, std::string& response, size_t maxBytes, bool withCredential = true);
  bool fetch(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& onData, bool withCredential);
  // HTTP status of the last fetch (-1 transport failure, 429 while backing off).
  int lastFetchStatus = 0;
  // Outcome of the last request, for user-facing errors.
  project_stick::NetFailure lastFailure = project_stick::NetFailure::None;
  int lastFailureStatus = 0;
  void recordOutcome(bool clockReady, int status);

  // NimBLE heap lending (worker task only). A cloud operation releases NimBLE
  // when the heap cannot afford a TLS handshake, or after a transport failure,
  // and the outermost RadioLease brings it back when the operation ends.
  bool radioLent = false;
  bool leaseActive = false;
  bool lendRadioIfLow(const char* what);
  bool memorySkipped = false;
  bool lendRadioAfterFailure(const char* what);
  struct RadioLease {
    explicit RadioLease(ProjectStickService& service);
    ~RadioLease();
    RadioLease(const RadioLease&) = delete;
    RadioLease& operator=(const RadioLease&) = delete;
    ProjectStickService& service;
    bool outer;
  };
  bool parseServerTime(const char* value);
  bool hashFile(const std::string& path, std::string& result);
  bool flushEvents();
  void reportFirmwareOutcome();
#ifdef SIMULATOR
  // Installs /.crosspoint/studio/import.ssp (written by the web simulator
  // loader) through the same StudioFrame path BLE uses.
  void importSimulatorProgram();
  void importSimulatorStream();
#endif
  static std::string makeUuid();
};
