#pragma once

#include <ProjectStickCore.h>
#include <ProjectStickSyncState.h>
#include <SecureHttpClient.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
  SyncReport sync(bool registerFirst = true);
  bool pollAlerts();
  void syncStudio();
  // Drops a kept-alive API connection (called by the worker when idle).
  void closeIdleConnection();

  // Device-side firmware check / request (Settings > Firmware update).
  struct FirmwareOffer {
    enum class Status : uint8_t { UpdateAvailable, UpToDate, Unbound, Failed, Requested, RequestFailed };
    Status status = Status::Failed;
    std::string id;
    std::string version;
    std::string notes;
  };
  FirmwareOffer checkFirmware();
  FirmwareOffer requestFirmware(const std::string& firmwareId);
  bool refreshOwnership();
  bool syncStudioCommand();
  void reportFirmwareOutcome();
  void sendStudioFeedback(const std::string& task, const std::string& card, bool useful);

  const Display& display() const { return currentDisplay; }
  Display displaySnapshot() const;
  void adoptDisplay(Display display);
  void adoptServerTime(const project_stick::ShanghaiTime& time);
  uint32_t pendingEventCount() const;
  uint32_t pollIntervalSeconds() const;
  uint32_t alertPollIntervalSeconds() const;
  uint32_t studioPollSeconds() const;
  // Shared cloud backoff (429 / 5xx / unreachable): no request goes out while
  // blocked. rateLimited() is the server-requested (429) part; a manual sync
  // may clear an error backoff but never a rate limit.
  static bool apiBlocked();
  static bool apiRateLimited();
  static bool clearBackoffForManualSync();
  bool isTradingDay() const;
  bool hasClock() const { return serverTime.valid; }
  bool isBound() const;
  std::string pairingCode() const;
  std::string deviceId() const;
  // Stores the credential a phone delivered over BLE setup (protocol 3):
  // device token, owner and bound=true. Any registration already in flight
  // with the previous identity state is discarded instead of applied.
  bool applyBleBinding(const std::string& deviceToken, const std::string& owner);
  project_stick::ShanghaiTime now() const;

 private:
  std::string baseUrl, localOwner, studioAttemptTask;
  uint8_t studioAttempts = 0;
  uint32_t studioRetryAt = 0;
  void adoptOwner(const std::string& owner);
  Display currentDisplay;
  project_stick::ShanghaiTime serverTime;
  uint32_t serverTimeCapturedMs = 0;
  bool inactive = false;
  // One client per service keeps the Cloudflare TLS connection alive across
  // the register, Studio and event requests of one sync burst.
  freeink::SecureHttpClient http;

  bool ensureIdentity();
  bool ensurePairing(int& status);
  bool registerDevice(int& status);
  enum class DownloadResult : uint8_t { Complete, Retry, Fatal };
  DownloadResult downloadFirmware(const std::string& id, const std::string& hash, size_t size);
  uint32_t firmwareRetryAtMs = 0;
  bool requestPost(const std::string& path, const std::string& body, std::string& response, int& status);
  bool fetchJson(const std::string& url, std::string& response, size_t maxBytes);
  bool fetchAuthenticated(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& onData);
  // HTTP status of the last fetchAuthenticated (-1 transport failure, 429 while backing off).
  int lastFetchStatus = 0;
  bool parseServerTime(const char* value);
  bool hashFile(const std::string& path, std::string& result);
  bool flushEvents();
  static std::string makeUuid();
};
