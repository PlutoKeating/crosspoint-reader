#include "ProjectStickService.h"

#include <HalPowerManager.h>

#include "FirmwareInstall.h"
#include "FirmwareUpdateState.h"
#include "StudioBluetooth.h"
#include "StudioFrame.h"
#include "StudioReceiver.h"
#include "network/FirmwareFlasher.h"
#include "network/OtaTrial.h"
// WiFi.h also has a simulator shim; the trial health signal needs link state on both.
#include <WiFi.h>
#ifndef SIMULATOR
#include <sys/time.h>

#include "StudioTrust.h"
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ProjectStickApiBackoff.h>
#include <ProjectStickWifiPolicy.h>
#include <SecureHttpClient.h>
#include <StickFirmware.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#ifdef SIMULATOR
#include <random>
#endif
#include <utility>

#include "project_stick/ProjectStickStore.h"

#ifndef PROJECT_STICK_BASE_URL
#define PROJECT_STICK_BASE_URL "https://stockstick.plutokeating.beer"
#endif

namespace {
std::recursive_mutex projectStickStateMutex;
using ProjectStickStateLock = std::lock_guard<std::recursive_mutex>;
bool projectStickStoreInitialized = false;
// Bumped when a BLE bind replaces the device token/owner; registration
// responses started before the bump are dropped.
std::atomic<uint32_t> bleBindingGeneration{0};

// One backoff gate for every cloud request, shared by the UI-side and the
// background service instances: an overloaded or rate-limiting server (429,
// 5xx, unreachable) is left alone for 30 s .. 10 min instead of being retried.
std::mutex apiBackoffMutex;
project_stick::ApiBackoff apiBackoff;

void noteApiResult(int status, const std::string& retryAfter) {
  std::lock_guard<std::mutex> lock(apiBackoffMutex);
  const uint32_t retryAfterSeconds = project_stick::parseRetryAfterSeconds(retryAfter.c_str());
  const bool wasBlocked = apiBackoff.failures() > 0;
  apiBackoff.record(millis(), status, retryAfterSeconds);
  if (project_stick::isBackoffStatus(status))
    LOG_ERR("STICK", "API status %d: backing off (failures=%u retry-after=%us)", status,
            (unsigned)apiBackoff.failures(), (unsigned)retryAfterSeconds);
  else if (wasBlocked)
    LOG_INF("STICK", "API reachable again; backoff cleared");
}

// The simulator's HTTP shim exposes no response headers; it falls back to the
// exponential backoff on 429.
std::string retryAfterHeader(freeink::SecureHttpClient& client) {
#ifdef SIMULATOR
  (void)client;
  return "";
#else
  return client.getHeader("retry-after");
#endif
}

bool apiBlockedNow() {
  std::lock_guard<std::mutex> lock(apiBackoffMutex);
  return apiBackoff.blocked(millis());
}

// Firmware identity and capabilities sent with registration.
void describeFirmware(JsonDocument& request) {
  request["firmware_version"] = CROSSPOINT_VERSION;
  request["firmware_build"] = firmware_install::runningBuild();
  JsonObject capabilities = request["capabilities"].to<JsonObject>();
  // Content over BLE only: protocol 2 frames/programs, protocol 3 setup
  // (bind, Wi-Fi) and BLE-triggered OTA (resumable download of the catalogue
  // image, identity checks, trial boot with automatic rollback).
  capabilities["ble"] = 3;
  capabilities["ota"] = 3;
  capabilities["panel"] = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
}

// Field telemetry: the next heap or radio problem is visible server-side.
// Names match the register route's accepted metrics; the BLE STATE
// characteristic carries the same object so the phone can relay it.
void describeMetrics(JsonObject metrics) {
  metrics["heap_free"] = ESP.getFreeHeap();
  metrics["heap_min"] = ESP.getMinFreeHeap();
  metrics["heap_max_alloc"] = ESP.getMaxAllocHeap();
  metrics["uptime_ms"] = millis();
  metrics["wifi_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  metrics["wifi_on"] = WiFi.getMode() != WIFI_MODE_NULL ? 1 : 0;  // Wi-Fi is on demand since 2.7.2
  metrics["battery_percent"] = powerManager.getBatteryPercentage();
  // Card usage comes from the worker's idle scan (StudioFrame::refreshStorageUsage);
  // nothing here touches the FAT.
  uint64_t total = 0, used = 0;
  if (StudioFrame::storageUsage(total, used)) {
    metrics["sd_total"] = total;
    metrics["sd_used"] = used;
  }
}

// Unix seconds when the system clock is trusted, else from the server clock, else 0.
int64_t trustedUnixNow(const project_stick::ShanghaiTime& fallback) {
  const std::time_t wall = std::time(nullptr);
  if (wall >= 1735689600) return static_cast<int64_t>(wall);
  return fallback.valid ? fallback.day * 86400LL + fallback.secondOfDay - 8 * 3600 : 0;
}

bool trustedClockReady() {
#ifndef SIMULATOR
  if (std::time(nullptr) >= 1735689600) return true;
  static bool requested = false;
  if (!requested) {
    configTime(0, 0, "time.cloudflare.com", "time.google.com");
    requested = true;
  }
  const uint32_t deadline = millis() + 10000;
  while (std::time(nullptr) < 1735689600 && static_cast<int32_t>(millis() - deadline) < 0) delay(100);
  return std::time(nullptr) >= 1735689600;
#else
  return true;
#endif
}

constexpr size_t MAX_ALERT_BYTES = 32 * 1024;

// True when a block of `bytes` (plus headroom for the TLS session that is
// running) can be allocated; used before growing heap strings in the worker.
bool heapFits(size_t bytes) {
#ifndef SIMULATOR
  return ESP.getMaxAllocHeap() >= bytes + 4096;
#else
  (void)bytes;
  return true;
#endif
}
constexpr size_t IO_CHUNK = 1024;
// Bounds each stage of a request: TCP connect, TLS handshake (SecureClient
// keeps a 15 s floor) and every response read. With the TLS 1.2 retry off, a
// failed attempt costs at most connect + handshake = 30 s, and the 60 s UI
// deadlines in FirmwareUpdateActivity always see an outcome first.
constexpr uint32_t HTTP_TIMEOUT_MS = 15000;

class HttpBurst {
 public:
  explicit HttpBurst(freeink::SecureHttpClient& client) : client(client) {}
  ~HttpBurst() { client.end(); }

 private:
  freeink::SecureHttpClient& client;
};

bool validSha256(const std::string& value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

}  // namespace

void ProjectStickService::begin() {
  baseUrl = PROJECT_STICK_BASE_URL;
  while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
  http.setTimeout(HTTP_TIMEOUT_MS);
#ifdef SIMULATOR
  http.setInsecure();
#else
  http.setCACert(PROJECT_STICK_ROOT_CA);
#endif
#ifndef SIMULATOR
  http.setUserAgent("Project.Stick-CrossPoint-" CROSSPOINT_VERSION);
  http.setFollowRedirects(3);
  // The StockStick API (Cloudflare) speaks TLS 1.3; a TLS 1.2 retry after a
  // failed handshake only doubled each failure (~40 s) and its heap churn.
  http.setTls12Fallback(false);
#endif
  ProjectStickStateLock lock(projectStickStateMutex);
  if (!projectStickStoreInitialized) {
    loadStore();
    projectStickStoreInitialized = true;
  }
  localOwner = PROJECT_STICK_STORE.ownerId;
  StudioFrame::instance().load();
#ifdef SIMULATOR
  importSimulatorProgram();
  importSimulatorStream();
#endif
  currentDisplay.alertUntil = PROJECT_STICK_STORE.alertUntil;
}

bool ProjectStickService::ensureIdentity() {
  if (!PROJECT_STICK_STORE.deviceId.empty()) return false;
  PROJECT_STICK_STORE.deviceId = makeUuid();
  LOG_INF("STICK", "Created device identity %s", PROJECT_STICK_STORE.deviceId.c_str());
  return true;
}

// Loads project_stick.json once at boot. Caller holds the state lock.
//
// Two rules keep the credential: a file that exists but cannot be read right
// now (SD hiccup, no memory for the parse) is retried and, failing that, left
// alone (the store refuses to save until a load succeeds), instead of being
// replaced by defaults with a fresh identity as 2.6.x did. And the BLE
// authority file is the second copy of who this device is: when the store
// lost its identity or binding while ble.json still has them (the split a
// starved-heap save produced in 2.6.x), identity and owner come back from
// there. Only the cloud token is gone then; the phone restores it with the
// BLE `token` op.
void ProjectStickService::loadStore() {
  using LoadState = PersistableStoreBase::LoadState;
  auto& s = PROJECT_STICK_STORE;
  bool loaded = s.loadFromFile();
  for (int attempt = 0; !loaded && s.lastLoadState() == LoadState::Unavailable && attempt < 3; ++attempt) {
    delay(100);
    loaded = s.loadFromFile();
  }
  const bool unavailable = s.lastLoadState() == LoadState::Unavailable;
  if (unavailable) LOG_ERR("STICK", "Device store unreadable; keeping it on the card untouched");
  bool changed = false;
  std::string bleIdentity, bleOwner;
  if (studio_ble::storedAuthority(bleIdentity, bleOwner)) {
    if (s.deviceId != bleIdentity) {
      LOG_ERR("STICK", "Store identity %s differs from the BLE authority %s; adopting the authority",
              s.deviceId.empty() ? "(none)" : s.deviceId.c_str(), bleIdentity.c_str());
      s.deviceId = bleIdentity;
      changed = true;
    }
    if (!s.bound || s.ownerId != bleOwner) {
      LOG_ERR("STICK", "Store lost the binding the BLE authority holds (owner %s); restoring it", bleOwner.c_str());
      s.bound = true;
      s.ownerId = bleOwner;
      changed = true;
    }
  }
  if (!unavailable && (ensureIdentity() || changed)) s.saveToFile();
  if (!s.deviceToken.empty() && !s.bound) {
    // Not a state the code writes; a token without a binding is useless.
    s.deviceToken.clear();
  }
}

// Wi-Fi is on demand (ProjectStickHost brings it up for a queued cloud job):
// a job waits for the link before its first request.
bool ProjectStickService::awaitWifi(const char* what) {
  const uint32_t startedMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startedMs >= project_stick::WIFI_JOB_WAIT_MS) {
      LOG_ERR("STICK", "%s: Wi-Fi did not come up in %u s", what, (unsigned)(project_stick::WIFI_JOB_WAIT_MS / 1000));
      lastFailure = project_stick::NetFailure::Network;
      lastFailureStatus = 0;
      return false;
    }
    delay(200);
  }
  return true;
}

ProjectStickService::RadioLease::RadioLease(ProjectStickService& service)
    : service(service), outer(!service.leaseActive) {
  service.leaseActive = true;
}

ProjectStickService::RadioLease::~RadioLease() {
  if (!outer) return;
  service.leaseActive = false;
  if (service.radioLent) {
    studio_ble::restoreRadio();
    service.radioLent = false;
  }
}

// Before a TLS connection: if the heap cannot afford the handshake, lend
// NimBLE's heap for the rest of the cloud operation. Returns false when even
// then the heap is below the hard floor: the caller skips the request.
bool ProjectStickService::lendRadioIfLow(const char* what) {
#ifndef SIMULATOR
  uint32_t freeHeap = ESP.getFreeHeap(), maxAlloc = ESP.getMaxAllocHeap();
  LOG_INF("STICK", "%s: heap=%u max=%u min=%u%s", what, (unsigned)freeHeap, (unsigned)maxAlloc,
          (unsigned)ESP.getMinFreeHeap(), radioLent ? " (BLE released)" : "");
  if (!radioLent && !project_stick::tlsHeapSufficient(freeHeap, maxAlloc) && studio_ble::releaseRadio()) {
    radioLent = true;
    freeHeap = ESP.getFreeHeap();
    maxAlloc = ESP.getMaxAllocHeap();
    LOG_INF("STICK", "%s: heap below TLS needs; BLE released (heap=%u max=%u)", what, (unsigned)freeHeap,
            (unsigned)maxAlloc);
  }
  if (project_stick::tlsHeapAffordable(freeHeap, maxAlloc)) return true;
  LOG_ERR("STICK", "%s skipped: low memory (heap=%u max=%u)", what, (unsigned)freeHeap, (unsigned)maxAlloc);
  lastFailure = project_stick::NetFailure::Memory;
  lastFailureStatus = 0;
  return false;
#else
  (void)what;
  return true;
#endif
}

// After a transport failure: retry with NimBLE released, since an allocation
// failure inside the handshake looks like any other transport failure.
bool ProjectStickService::lendRadioAfterFailure(const char* what) {
  if (radioLent || !studio_ble::releaseRadio()) return false;
  radioLent = true;
  LOG_INF("STICK", "%s: transport failed; retrying with BLE released", what);
  return true;
}

void ProjectStickService::recordOutcome(const bool clockReady, const int status) {
#ifndef SIMULATOR
  const bool heapLow = !project_stick::tlsHeapSufficient(ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#else
  const bool heapLow = false;
#endif
  lastFailure = project_stick::classifyRequest(clockReady, status, heapLow);
  lastFailureStatus = status;
}

ProjectStickService::SyncReport ProjectStickService::sync() {
  RadioLease lease(*this);
  HttpBurst burst(http);
  const uint32_t syncStartedMs = millis();
  SyncReport report;
  inactive = false;
  if (!hasCredential()) {
    report.result = SyncResult::Unbound;
    return report;
  }
  if (!awaitWifi("sync")) {
    report.result = SyncResult::Failed;
    return report;
  }
  report.registerAttempted = true;
  int status = 0;
  if (!registerDevice(status)) {
    inactive = status == 403;
    report.result = inactive ? SyncResult::Inactive : SyncResult::Failed;
    return report;
  }
  report.registerSucceeded = true;
  // Registration returns the server clock; hand it to the foreground.
  report.synchronizedAt = now();
  if (!isBound()) {
    report.result = SyncResult::Unbound;
    return report;
  }
  if (ota_trial::hasPendingOutcome()) reportFirmwareOutcome();
  flushEvents();
  report.result = SyncResult::Synced;
  LOG_INF("STICK", "Sync finished (total=%ums)", (unsigned)(millis() - syncStartedMs));
  return report;
}

bool ProjectStickService::registerDevice(int& status) {
  LOG_INF("STICK", "Registering device (firmware=%s)", CROSSPOINT_VERSION);
  const uint32_t generation = bleBindingGeneration.load();
  JsonDocument request;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    request["device_id"] = PROJECT_STICK_STORE.deviceId;
  }
  describeFirmware(request);
  describeMetrics(request["metrics"].to<JsonObject>());
  std::string body;
  serializeJson(request, body);

  std::string response;
  if (!requestPost("/api/v2/device/register", body, response, status) || status != 200) {
    LOG_ERR("STICK", "Device registration failed (status=%d)", status);
    // The credential is gone server-side: re-bind over BLE setup.
    if (status == 401 && generation == bleBindingGeneration.load()) dropBinding();
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    LOG_ERR("STICK", "Device registration returned invalid JSON");
    return false;
  }

  uint32_t pollSeconds = 300;
  uint32_t alertSeconds = 30;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    if (generation != bleBindingGeneration.load()) {
      LOG_INF("STICK", "Registration raced a BLE bind; discarding the response");
      status = 0;
      return false;
    }
    PROJECT_STICK_STORE.pollIntervalSeconds = std::clamp<uint32_t>(doc["poll_interval_seconds"] | 300, 30, 86400);
    PROJECT_STICK_STORE.alertPollIntervalSeconds =
        std::clamp<uint32_t>(doc["alert_poll_interval_seconds"] | 30, 10, 3600);
    PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | false;
    parseServerTime(doc["server_time"] | "");
    if (!(doc["bound"] | false)) {
      LOG_INF("STICK", "Device was unbound in the cloud; back to BLE setup");
      dropBinding();
      return true;
    }
    const std::string owner = doc["owner_id"] | "";
    adoptOwner(owner);
    // Register is how a bound device learns a rotated BLE authority.
    const std::string key = doc["bluetooth"]["secret"] | "";
    const int64_t keyEpoch = doc["bluetooth"]["epoch"] | int64_t(0);
    if (ble_setup::validAuthority(key, keyEpoch) &&
        studio_ble::adoptAuthority(PROJECT_STICK_STORE.deviceId, key, static_cast<uint32_t>(keyEpoch), owner))
      LOG_INF("STICK", "Adopted BLE authority epoch %u", static_cast<unsigned>(keyEpoch));
    pollSeconds = PROJECT_STICK_STORE.pollIntervalSeconds;
    alertSeconds = PROJECT_STICK_STORE.alertPollIntervalSeconds;
    if (!PROJECT_STICK_STORE.saveToFile()) {
      LOG_ERR("STICK", "Device registration state could not be saved to SD");
      return false;
    }
  }
  LOG_INF("STICK", "Device registered (poll=%us alerts=%us)", (unsigned)pollSeconds, (unsigned)alertSeconds);
  return true;
}

// A-share continuous trading (Shanghai 09:30–11:30, 13:00–15:00) on a trading
// day; alerts only change inside it, so nothing polls outside.
bool ProjectStickService::inAlertWindow() const {
  project_stick::ShanghaiTime current;
  bool eligible = false;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    current = now();
    eligible = PROJECT_STICK_STORE.tradingDay && current.valid;
  }
  const uint16_t minute = current.minuteOfDay();
  eligible = eligible && ((minute >= 570 && minute < 690) || (minute >= 780 && minute < 900));
#ifdef SIMULATOR
  eligible = eligible || std::getenv("CROSSPOINT_SIM_FORCE_ALERT_WINDOW") != nullptr;
#endif
  return eligible;
}

bool ProjectStickService::pollAlerts() {
  if (!inAlertWindow() || !hasCredential()) return false;
  if (!awaitWifi("alerts")) return false;
  std::string deviceId;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceId = PROJECT_STICK_STORE.deviceId;
  }

  RadioLease lease(*this);
  HttpBurst burst(http);
  std::string response;
  const std::string url = baseUrl + "/api/v2/device/alerts?device_id=" + deviceId;
  if (!fetchJson(url, response, MAX_ALERT_BYTES)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;

  // Only the alert window is kept: the installed Studio program owns the alert
  // scene and StudioFrame::tick shows it while alertUntil lies in the future.
  ProjectStickStateLock lock(projectStickStateMutex);
  parseServerTime(doc["server_time"] | "");
  const bool wasTradingDay = PROJECT_STICK_STORE.tradingDay;
  PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | PROJECT_STICK_STORE.tradingDay;
  bool received = false;
  for (JsonObjectConst alert : doc["alerts"].as<JsonArrayConst>()) {
    const int64_t id = alert["id"] | 0;
    if (id == 0 || PROJECT_STICK_STORE.hasSeenAlert(id)) continue;
    project_stick::ShanghaiTime activeUntil;
    if (!project_stick::parseIso8601ToShanghai(alert["active_until"] | "", activeUntil)) continue;
    PROJECT_STICK_STORE.markAlertSeen(id);
    const auto& until = PROJECT_STICK_STORE.alertUntil;
    if (!until.valid || activeUntil.day > until.day ||
        (activeUntil.day == until.day && activeUntil.secondOfDay > until.secondOfDay)) {
      PROJECT_STICK_STORE.alertUntil = activeUntil;
      currentDisplay.alertUntil = activeUntil;
    }
    received = true;
  }
  // The store only hits the card when something changed (every 30 s otherwise).
  if (received || wasTradingDay != PROJECT_STICK_STORE.tradingDay) PROJECT_STICK_STORE.saveToFile();
  return received;
}





bool ProjectStickService::flushEvents() {
  std::vector<ProjectStickEvent> pending;
  std::string deviceId;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    if (PROJECT_STICK_STORE.pendingEvents.empty()) return true;
    pending = PROJECT_STICK_STORE.pendingEvents;
    deviceId = PROJECT_STICK_STORE.deviceId;
  }
  JsonDocument request;
  request["device_id"] = deviceId;
  JsonArray events = request["events"].to<JsonArray>();
  for (const auto& event : pending) {
    JsonObject obj = events.add<JsonObject>();
    obj["client_event_id"] = event.id;
    obj["event_type"] = event.type;
    if (!event.clientTs.empty()) obj["client_ts"] = event.clientTs;
    if (!event.studioTask.empty()) {
      obj["payload"]["task_id"] = event.studioTask;
      obj["payload"]["card_id"] = event.studioCard;
    }
    if (!event.detail.empty()) obj["payload"]["detail"] = event.detail;
  }
  std::string body;
  serializeJson(request, body);
  std::string response;
  int status = 0;
  if (!requestPost("/api/v2/device/events", body, response, status) || status != 200) return false;
  ProjectStickStateLock lock(projectStickStateMutex);
  for (const auto& sent : pending) {
    auto& events = PROJECT_STICK_STORE.pendingEvents;
    events.erase(std::remove_if(events.begin(), events.end(),
                                [&sent](const ProjectStickEvent& event) { return event.id == sent.id; }),
                 events.end());
  }
  return PROJECT_STICK_STORE.saveToFile();
}

bool ProjectStickService::requestPost(const std::string& path, const std::string& body, std::string& response,
                                      int& status) {
  status = 0;
  std::string deviceToken;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceToken = PROJECT_STICK_STORE.deviceToken;
  }
  if (deviceToken.empty()) return false;  // no credential, no cloud requests
  if (!trustedClockReady()) {
    LOG_ERR("STICK", "POST %s skipped: no trusted time (NTP unreachable)", path.c_str());
    recordOutcome(false, 0);
    return false;
  }
#ifdef SIMULATOR
  static bool injectedRegisterFailure = false;
  if (!injectedRegisterFailure && path == "/api/v2/device/register" &&
      std::getenv("CROSSPOINT_SIM_FAIL_FIRST_REGISTER") != nullptr) {
    injectedRegisterFailure = true;
    LOG_ERR("STICK", "Simulator injected the first register failure");
    return false;
  }
#endif
  if (apiBlockedNow()) {
    LOG_INF("STICK", "POST %s skipped: API backoff active", path.c_str());
    recordOutcome(true, 429);
    return false;
  }
  if (!lendRadioIfLow(path.c_str())) {
    status = 0;
    return false;
  }
  // One extra attempt, and only for a transport failure (dropped keep-alive,
  // Wi-Fi hiccup). A server answer, including 429/5xx, is never retried here:
  // the shared backoff decides when the next request may go out.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    const uint32_t attemptStartedMs = millis();
#ifndef SIMULATOR
    LOG_INF("STICK", "POST %s attempt %u/2 (heap=%u max=%u)", path.c_str(), (unsigned)attempt + 1,
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#else
    LOG_INF("STICK", "POST %s attempt %u/2 (simulator OpenSSL)", path.c_str(), (unsigned)attempt + 1);
#endif
    if (!http.begin(baseUrl + path)) {
      LOG_ERR("STICK", "POST %s has an invalid URL", path.c_str());
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + deviceToken);
    status = http.sendRequest("POST", body);
    ota_trial::noteApiResult(status, WiFi.status() == WL_CONNECTED);
    response = http.getString();
#ifdef SIMULATOR
    const bool complete = true;
#else
    const bool complete = http.responseComplete();
#endif
    recordOutcome(true, status > 0 && complete ? status : -1);
    if (status > 0 && complete) {
      noteApiResult(status, retryAfterHeader(http));
      LOG_INF("STICK", "POST %s completed (status=%d bytes=%u time=%ums)", path.c_str(), status,
              (unsigned)response.size(), (unsigned)(millis() - attemptStartedMs));
      // 429 and 5xx are answers too, but callers treat them as failures.
      return status != 429 && status < 500;
    }
    LOG_ERR("STICK", "POST %s failed (status=%d complete=%u bytes=%u)", path.c_str(), status, complete ? 1u : 0u,
            (unsigned)response.size());
    if (attempt == 0 && !lendRadioAfterFailure(path.c_str())) delay(500);
    if (attempt == 0 && !lendRadioIfLow(path.c_str())) break;  // below the hard floor: retrying only fragments
  }
  noteApiResult(status > 0 ? status : -1, "");
  return false;
}

bool ProjectStickService::fetchJson(const std::string& url, std::string& response, size_t maxBytes,
                                    const bool withCredential) {
  // Same policy as requestPost: one retry, for transport failures only.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    response.clear();
    if (!heapFits(std::min<size_t>(maxBytes, 4096))) {
      LOG_ERR("STICK", "GET skipped: no room for the response buffer");
      lastFailure = project_stick::NetFailure::Memory;
      break;
    }
    response.reserve(std::min<size_t>(maxBytes, 4096));
    const auto collect = [&response, maxBytes](const uint8_t* data, size_t length) {
      if (length > maxBytes - response.size()) return false;
      // std::string growth throws (abort under -fno-exceptions): stop the
      // transfer instead when the doubled buffer would not fit.
      const size_t needed = response.size() + length;
      if (needed > response.capacity() && !heapFits(std::max(needed, response.capacity() * 2))) return false;
      response.append(reinterpret_cast<const char*>(data), length);
      return true;
    };
    if (fetch(url, collect, withCredential)) return true;
    if (lastFetchStatus > 0 || lastFailure == project_stick::NetFailure::Clock || memorySkipped) break;
    if (attempt == 0 && !lendRadioAfterFailure("GET")) delay(500);
  }
  response.clear();
  return false;
}


// One GET. With `withCredential` it carries the device bearer and is refused
// without one (device API); without, it is an anonymous request to a public
// endpoint that any device may make (the firmware catalogue).
bool ProjectStickService::fetch(const std::string& url, const std::function<bool(const uint8_t*, size_t)>& onData,
                                const bool withCredential) {
  std::string deviceToken;
  if (withCredential) {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceToken = PROJECT_STICK_STORE.deviceToken;
  }
  lastFetchStatus = 0;
  if (withCredential && deviceToken.empty()) return false;  // no credential, no device API requests
  if (!trustedClockReady()) {
    LOG_ERR("STICK", "GET skipped: no trusted time (NTP unreachable)");
    recordOutcome(false, 0);
    return false;
  }
  if (apiBlockedNow()) {
    lastFetchStatus = 429;  // not a transport failure: do not retry
    recordOutcome(true, 429);
    return false;
  }
  memorySkipped = false;
  if (!lendRadioIfLow("GET")) {
    memorySkipped = true;
    return false;
  }
  if (!http.begin(url)) return false;
  // Pinned-root client; the ESP-IDF HTTP client (and the mbedTLS TLS stack it
  // pulls in) is not linked.
  if (withCredential) http.addHeader("Authorization", "Bearer " + deviceToken);
  const int status = http.GET([this, &onData](const uint8_t* data, size_t length) {
    if (http.getStatus() != 200) return true;
    return onData(data, length);
  });
  ota_trial::noteApiResult(status, WiFi.status() == WL_CONNECTED);
#ifdef SIMULATOR
  const bool complete = true;
#else
  const bool complete = http.responseComplete();
#endif
  const bool success = status == 200 && complete && !http.callbackAborted();
  // A body cut short by the network counts as a transport failure; a local
  // abort (storage full) says nothing about the server.
  lastFetchStatus = status > 0 && !complete && !http.callbackAborted() ? -1 : status;
  recordOutcome(true, lastFetchStatus);
  if (lastFetchStatus <= 0) LOG_ERR("STICK", "GET failed (status=%d)", status);
  noteApiResult(lastFetchStatus, retryAfterHeader(http));
  return success;
}




namespace {
constexpr std::time_t MIN_TRUSTED_UTC = 1735689600;  // 2025-01-01T00:00:00Z

int64_t utcFromShanghai(const project_stick::ShanghaiTime& time) {
  return time.valid ? time.day * 86400LL + time.secondOfDay - 8 * 3600 : 0;
}

// The system clock is the single trusted wall clock: set from NTP, the
// phone (BLE begin) or the server, and copied to the RTC by syncClock().
void adoptSystemTime(int64_t utc) {
#ifdef SIMULATOR
  (void)utc;  // the host clock is authoritative in the simulator
#else
  if (utc < MIN_TRUSTED_UTC) return;
  const std::time_t current = std::time(nullptr);
  if (current >= MIN_TRUSTED_UTC && std::llabs(static_cast<long long>(current - utc)) < 30) return;
  timeval tv{static_cast<time_t>(utc), 0};
  settimeofday(&tv, nullptr);
#endif
}
}  // namespace

bool ProjectStickService::parseServerTime(const char* value) {
  project_stick::ShanghaiTime parsed;
  if (!project_stick::parseIso8601ToShanghai(value, parsed)) return false;
  serverTime = parsed;
  serverTimeCapturedMs = millis();
  adoptSystemTime(utcFromShanghai(parsed));
  return true;
}

// Allocation-free (runs from the UI loop). Preference: server time, then the
// trusted system clock, then the RTC. An RTC that lost power reads
// 2000-01-01; such readings are rejected, and an invalid time makes
// StudioFrame::tick() hold the current frame instead of picking a schedule.
project_stick::ShanghaiTime ProjectStickService::now() const {
  if (serverTime.valid) return project_stick::advanceTime(serverTime, (millis() - serverTimeCapturedMs) / 1000);
  project_stick::ShanghaiTime result;
  const std::time_t wall = std::time(nullptr);
  std::tm utc{};
  if (wall >= MIN_TRUSTED_UTC && gmtime_r(&wall, &utc) &&
      project_stick::shanghaiFromUtc(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                                     utc.tm_sec, result)) {
    return result;
  }
#ifndef SIMULATOR
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (halClock.getDateTime(year, month, day, hour, minute, second))
    project_stick::shanghaiFromUtc(year, month, day, hour, minute, second, result);
#endif
  return result;
}

// Main-loop only: the RTC shares the I2C bus with the UI task's other
// peripherals, so it is never touched from the sync worker or BLE callbacks.
void ProjectStickService::syncClock() {
#ifndef SIMULATOR
  if (!halClock.isAvailable()) return;
  const uint32_t nowMs = millis();
  if (lastClockSyncMs != 0 && nowMs - lastClockSyncMs < CLOCK_SYNC_MS) return;
  lastClockSyncMs = nowMs;
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
  project_stick::ShanghaiTime rtc;
  const bool rtcTrusted = halClock.getDateTime(year, month, day, hour, minute, second) &&
                          project_stick::shanghaiFromUtc(year, month, day, hour, minute, second, rtc);
  const std::time_t wall = std::time(nullptr);
  if (wall >= MIN_TRUSTED_UTC) {
    if (!rtcTrusted || std::llabs(static_cast<long long>(utcFromShanghai(rtc) - wall)) > 120) halClock.setUtc(wall);
  } else if (rtcTrusted) {
    adoptSystemTime(utcFromShanghai(rtc));  // lets TLS start without waiting for NTP
  }
#endif
}

bool ProjectStickService::hashFile(const std::string& path, std::string& result) {
  HalFile file;
  if (!Storage.openFileForRead("STICK", path, file)) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(IO_CHUNK);
  if (!buffer) {
    LOG_ERR("STICK", "OOM: SHA-256 buffer");
    return false;
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  mbedtls_sha256_starts(&context, 0);
  while (file.available()) {
    const int count = file.read(buffer.get(), IO_CHUNK);
    if (count <= 0) {
      mbedtls_sha256_free(&context);
      return false;
    }
    mbedtls_sha256_update(&context, buffer.get(), static_cast<size_t>(count));
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  char hex[65];
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  hex[64] = '\0';
  result = hex;
  return true;
}









std::string ProjectStickService::makeUuid() {
  uint8_t bytes[16];
#ifdef SIMULATOR
  static std::mt19937 random(std::random_device{}());
  for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
#else
  esp_fill_random(bytes, sizeof(bytes));
#endif
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
  char value[37];
  snprintf(value, sizeof(value), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
           bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
           bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return value;
}

ProjectStickService::Display ProjectStickService::displaySnapshot() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return currentDisplay;
}

void ProjectStickService::adoptDisplay(Display display) {
  ProjectStickStateLock lock(projectStickStateMutex);
  currentDisplay = std::move(display);
}

void ProjectStickService::adoptServerTime(const project_stick::ShanghaiTime& time) {
  if (!time.valid) return;
  ProjectStickStateLock lock(projectStickStateMutex);
  serverTime = time;
  serverTimeCapturedMs = millis();
}

uint32_t ProjectStickService::pendingEventCount() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return static_cast<uint32_t>(PROJECT_STICK_STORE.pendingEvents.size());
}
uint32_t ProjectStickService::pollIntervalSeconds() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.pollIntervalSeconds;
}
bool ProjectStickService::apiBlocked() { return apiBlockedNow(); }
bool ProjectStickService::clearBackoffForManualSync() {
  std::lock_guard<std::mutex> lock(apiBackoffMutex);
  return apiBackoff.clearForManual(millis());
}
uint32_t ProjectStickService::alertPollIntervalSeconds() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.alertPollIntervalSeconds;
}
bool ProjectStickService::isTradingDay() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.tradingDay;
}

bool ProjectStickService::isBound() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.bound;
}

bool ProjectStickService::hasCredential() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.bound && !PROJECT_STICK_STORE.deviceToken.empty();
}

std::string ProjectStickService::deviceId() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.deviceId;
}

// The store half of a BLE bind failed to stick on the BLE side: back out, so
// the device is not bound in the cloud while unreachable over BLE.
void ProjectStickService::undoBleBinding() {
  ProjectStickStateLock lock(projectStickStateMutex);
  bleBindingGeneration.fetch_add(1);
  auto& s = PROJECT_STICK_STORE;
  s.bound = false;
  s.deviceToken.clear();
  s.ownerId.clear();
  s.saveToFile();
  refreshOwnership();
  LOG_ERR("STICK", "BLE bind rolled back: the BLE credential could not be written");
}

bool ProjectStickService::applyBleBinding(const std::string& deviceToken, const std::string& owner) {
  ProjectStickStateLock lock(projectStickStateMutex);
  bleBindingGeneration.fetch_add(1);
  auto& s = PROJECT_STICK_STORE;
  if (s.ownerId != owner) {
    // Same reset as adoptOwner, minus revoke(): the BLE credential for this
    // owner arrives with the bind and is persisted by studio_ble.
    StudioFrame::instance().clear();
    s.alertUntil = {};
    s.pendingEvents.clear();
    s.seenAlertIds.clear();
  }
  s.ownerId = owner;
  s.deviceToken = deviceToken.substr(0, 96);
  s.bound = true;
  const bool saved = s.saveToFile();
  LOG_INF("STICK", "Bound over BLE (owner=%s saved=%u)", owner.c_str(), saved ? 1u : 0u);
  return saved;
}


namespace {
constexpr char FIRMWARE_TEMP[] = "/.crosspoint/studio/firmware.tmp";
constexpr char FIRMWARE_META[] = "/.crosspoint/studio/firmware.meta";
// Both OTA slots in partitions.csv are 0x640000 bytes.
constexpr size_t MAX_FIRMWARE_BYTES = 0x640000;
constexpr size_t MIN_FIRMWARE_BYTES = 100000;
constexpr uint8_t MIN_OTA_BATTERY_PERCENT = 30;
// How long an install waits for a running content transfer to finish.
constexpr uint32_t TRANSFER_WAIT_MS = 30000;

size_t fileSize(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}
}  // namespace

// Only catalogue images hosted by the StockStick website are downloaded, over
// the same pinned-root TLS client as the API.
bool ProjectStickService::validFirmwareTarget(const FirmwareTarget& target) const {
  const std::string prefix = baseUrl + "/firmware/";
  return !target.version.empty() && target.version.size() <= 32 && validSha256(target.sha256) &&
         target.bytes >= MIN_FIRMWARE_BYTES && target.bytes <= MAX_FIRMWARE_BYTES && target.url.size() < 256 &&
         target.url.compare(0, prefix.size(), prefix) == 0 && target.url.find_first_of(" #?\r\n") == std::string::npos;
}

void ProjectStickService::installFirmware(const FirmwareTarget& target) {
#ifndef SIMULATOR
  // A new image is still proving itself; installing now would overwrite the
  // slot it rolls back to.
  if (ota_trial::active()) {
    firmware_update::fail("trial_active");
    return;
  }
  // No binding or device token is needed: the image comes from the public
  // catalogue and is identified by its hash and its embedded descriptor.
  if (!validFirmwareTarget(target)) {
    firmware_update::fail("invalid_target");
    return;
  }
  // A flash must not die half way: below 30 % only while external power is
  // charging the battery (published by the main loop, which owns the gauge).
  if (powerManager.getBatteryPercentage() < MIN_OTA_BATTERY_PERCENT && !firmware_update::externalPower()) {
    firmware_update::fail("low_battery");
    return;
  }
  // A content transfer owns the SD card and the radio; give it time to finish
  // instead of refusing right away.
  for (uint32_t waited = 0; StudioFrame::instance().busy(); waited += 250) {
    if (waited >= TRANSFER_WAIT_MS) {
      firmware_update::fail("device_busy");
      return;
    }
    delay(250);
  }
  // TLS and the flash writer need the heap NimBLE holds: release it for the
  // whole install (a success restarts; a failure restores it with the lease).
  RadioLease lease(*this);
  if (!radioLent && studio_ble::releaseRadio()) radioLent = true;
  HalPowerManager::Lock powerLock;
  firmware_update::begin(target.version.c_str(), target.bytes);
  auto fail = [](const char* error) { firmware_update::fail(error); };
  // Wi-Fi is on demand: the host brings it up for this job; the image is
  // downloaded by the device itself.
  if (!awaitWifi("install")) return fail("download_failed");
  if (downloadFirmware(target) != DownloadResult::Complete) return fail("download_failed");

  firmware_update::setPhase(firmware_update::Phase::Verifying);
  std::string downloadedHash;
  if (fileSize(FIRMWARE_TEMP) != target.bytes || !hashFile(FIRMWARE_TEMP, downloadedHash) ||
      downloadedHash != target.sha256) {
    Storage.remove(FIRMWARE_TEMP);
    Storage.remove(FIRMWARE_META);
    return fail("checksum_mismatch");
  }
  const auto candidate = firmware_install::inspect(FIRMWARE_TEMP, target.version.c_str());
  if (!candidate.ok()) {
    Storage.remove(FIRMWARE_TEMP);
    Storage.remove(FIRMWARE_META);
    return fail(stick_fw::installVerdictName(candidate.verdict));
  }
  firmware_update::setPhase(firmware_update::Phase::Installing);
  auto onProgress = +[](size_t written, size_t, void*) { firmware_update::setProgress(written); };
  auto armTrial = +[](const esp_partition_t* dest, void* version) {
    return ota_trial::arm(dest, static_cast<const char*>(version));
  };
  const auto result = firmware_flash::flashFromSdPath(FIRMWARE_TEMP, onProgress,
                                                      const_cast<char*>(target.version.c_str()), false, armTrial);
  if (result != firmware_flash::Result::OK) {
    ota_trial::disarm();  // the new image will not boot, so there is no trial
    return fail(firmware_flash::resultName(result));
  }
  Storage.remove(FIRMWARE_TEMP);
  Storage.remove(FIRMWARE_META);
  firmware_update::setPhase(firmware_update::Phase::Restarting);
  delay(1500);  // lets the UI and the phone see the restarting state
  ESP.restart();
#else
  (void)target;
  firmware_update::fail("unsupported");
#endif
}

ProjectStickService::DownloadResult ProjectStickService::downloadFirmware(const FirmwareTarget& target) {
  // A partial download survives errors, power loss and reboots: it is keyed on
  // the image (hash + size), and a later install of the same image resumes it.
  const std::string meta = target.sha256 + " " + std::to_string(target.bytes);
  {
    HalFile metaFile;
    char stored[128] = {};
    if (Storage.openFileForRead("STUDIO", FIRMWARE_META, metaFile)) {
      const int read = metaFile.read(stored, sizeof(stored) - 1);
      stored[read > 0 ? read : 0] = '\0';
    }
    metaFile.close();
    if (meta != stored || fileSize(FIRMWARE_TEMP) > target.bytes) {
      Storage.remove(FIRMWARE_TEMP);
      Storage.mkdir("/.crosspoint/studio");
      if (!Storage.openFileForWrite("STUDIO", FIRMWARE_META, metaFile) ||
          metaFile.write(reinterpret_cast<const uint8_t*>(meta.data()), meta.size()) != meta.size()) {
        return DownloadResult::Retry;
      }
      metaFile.close();
    }
  }

  HttpBurst burst(http);
  const size_t size = target.bytes;
  constexpr size_t FLUSH_INTERVAL = 256 * 1024;  // bounds what power loss can discard
  for (uint8_t attempt = 0; attempt < 4; ++attempt) {
    // The file on SD is the source of truth: a short write or an aborted pass
    // may have stored more (or fewer) bytes than the last callback counted.
    size_t offset = fileSize(FIRMWARE_TEMP);
    if (offset == size) return DownloadResult::Complete;
    firmware_update::setProgress(offset);
    if (offset > 0) LOG_INF("OTA", "Resuming firmware download at %u/%u", (unsigned)offset, (unsigned)size);
    if (!trustedClockReady()) return DownloadResult::Retry;
    HalFile output = Storage.open(FIRMWARE_TEMP, O_WRONLY | O_CREAT | O_APPEND);
    if (!output) return DownloadResult::Retry;
    if (!http.begin(target.url)) return DownloadResult::Fatal;
    if (offset > 0) http.addHeader("Range", "bytes=" + std::to_string(offset) + "-");
    bool restartFromZero = false;
    const size_t requested = offset;
    size_t unflushed = 0;
    const int status = http.GET([&](const uint8_t* data, size_t length) {
      const int code = http.getStatus();
      // A server that ignores Range answers 200 with the whole image.
      if (requested > 0 && code == 200) {
        restartFromZero = true;
        return false;
      }
      if (code != 200 && code != 206) return true;  // error body: drop it
      if (length > size - offset || output.write(data, length) != length) return false;
      offset += length;
      unflushed += length;
      if (unflushed >= FLUSH_INTERVAL) {
        output.flush();  // persists the directory entry size for resume
        unflushed = 0;
      }
      firmware_update::setProgress(offset);
      return true;
    });
    output.flush();
    output.close();
    ota_trial::noteApiResult(status, WiFi.status() == WL_CONNECTED);
    if (restartFromZero) {
      LOG_INF("OTA", "Server ignored Range; restarting download");
      Storage.remove(FIRMWARE_TEMP);
      continue;
    }
    // Withdrawn image or unsatisfiable range: resuming cannot help.
    if (status == 403 || status == 404 || status == 410 || status == 416) return DownloadResult::Fatal;
    LOG_INF("OTA", "Firmware download pass: status=%d stored %u/%u", status, (unsigned)fileSize(FIRMWARE_TEMP),
            (unsigned)size);
    if (attempt < 3) delay(1000UL << attempt);
  }
  return fileSize(FIRMWARE_TEMP) == size ? DownloadResult::Complete : DownloadResult::Retry;
}

// Settings > Firmware update. Needs Wi-Fi only: the public catalogue endpoint
// takes no credential, and the device itself decides whether the published
// version is newer than the one it runs. Nothing here reads or changes the
// binding, whatever the server answers.
ProjectStickService::FirmwareOffer ProjectStickService::checkFirmware() {
  FirmwareOffer offer;
  if (!awaitWifi("firmware check")) {
    offer.failure = project_stick::NetFailure::Network;
    return offer;
  }
  if (baseUrl.empty()) {  // builds without a cloud (the website simulator) never ask
    offer.failure = project_stick::NetFailure::Network;
    return offer;
  }
  RadioLease lease(*this);
  HttpBurst burst(http);
  std::string response;
  if (!fetchJson(baseUrl + "/api/v1/public/firmware/latest?channel=stable", response, 6144, false)) {
    if (lastFetchStatus == 404) {  // nothing published on this channel
      offer.status = FirmwareOffer::Status::UpToDate;
      return offer;
    }
    offer.failure = lastFailure == project_stick::NetFailure::None ? project_stick::NetFailure::Network : lastFailure;
    offer.httpStatus = lastFailureStatus;
    return offer;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    offer.failure = project_stick::NetFailure::Server;
    return offer;
  }
  offer.target.version = doc["version"] | "";
  offer.target.url = doc["url"] | "";
  if (offer.target.url.empty()) offer.target.url = doc["bin_url"] | "";
  offer.target.sha256 = doc["sha256"] | "";
  offer.target.bytes = doc["bytes"] | size_t(0);
  offer.notes = (doc["notes"] | "");
  if (offer.notes.size() > 600) offer.notes.resize(600);
  if (!validFirmwareTarget(offer.target)) {
    LOG_ERR("OTA", "Catalogue answer is not installable (version=%s)", offer.target.version.c_str());
    offer.failure = project_stick::NetFailure::Server;
    return offer;
  }
  const bool newer = stick_fw::isNewerVersion(offer.target.version.c_str(), CROSSPOINT_VERSION);
  LOG_INF("OTA", "Catalogue offers %s, running %s: %s", offer.target.version.c_str(), CROSSPOINT_VERSION,
          newer ? "update available" : "up to date");
  offer.status = newer ? FirmwareOffer::Status::UpdateAvailable : FirmwareOffer::Status::UpToDate;
  return offer;
}

// Confirmed installs show up as the new firmware_version at the next
// register; a rollback is reported as an event.
void ProjectStickService::reportFirmwareOutcome() {
  const auto outcome = ota_trial::pendingOutcome();
  if (outcome.pending && outcome.rolledBack) {
    ProjectStickStateLock lock(projectStickStateMutex);
    ProjectStickEvent event;
    event.id = makeUuid();
    event.type = "firmware_rolled_back";
    event.detail = std::string(outcome.version) + " " + outcome.reason;
    event.clientTs = project_stick::formatIso8601Shanghai(now());
    PROJECT_STICK_STORE.enqueue(std::move(event));
    PROJECT_STICK_STORE.saveToFile();
  }
  ota_trial::clearOutcome();
}

#ifdef SIMULATOR
void ProjectStickService::importSimulatorProgram() {
  constexpr char IMPORT[] = "/.crosspoint/studio/import.ssp";
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", IMPORT, file)) return;
  const size_t size = file.size();
  auto& frame = StudioFrame::instance();
  std::string hash;
  file.close();
  if (!hashFile(IMPORT, hash)) {
    Storage.remove(IMPORT);
    return;
  }
  // The task id only has to be a stable UUID-shaped handle for this content.
  const std::string task = hash.substr(0, 8) + "-" + hash.substr(8, 4) + "-4" + hash.substr(13, 3) + "-8" +
                           hash.substr(17, 3) + "-" + hash.substr(20, 12);
  bool installed = frame.snapshot().hash == hash;
  if (!installed && frame.start(task, hash, 0, size) && Storage.openFileForRead("STUDIO", IMPORT, file)) {
    auto buffer = makeUniqueNoThrow<uint8_t[]>(IO_CHUNK);
    size_t offset = 0;
    while (buffer && file.available()) {
      const int count = file.read(buffer.get(), IO_CHUNK);
      if (count <= 0 || !frame.append(offset, buffer.get(), static_cast<size_t>(count))) break;
      offset += static_cast<size_t>(count);
    }
    file.close();
    installed = offset == size && frame.commit();
    if (!installed) frame.abort(true);
  }
  Storage.remove(IMPORT);
  LOG_INF("STICK", "Simulator program import %s (%u bytes)", installed ? "installed" : "failed", (unsigned)size);
}

// Test hook for the BLE transfer protocol 4 receiver (no radio in the
// simulator): /.crosspoint/studio/import.v4 holds a decrypted stream and
// import.v4.json its begin4 fields {task, hash, size, header[, stop_at]}.
// The stream goes through the same StudioReceiver/StudioFrame path as BLE.
// With stop_at, the first pass stops there (link lost), then resumes from
// the receiver's resume point with the stream's remaining records.
void ProjectStickService::importSimulatorStream() {
  constexpr char STREAM[] = "/.crosspoint/studio/import.v4";
  constexpr char META[] = "/.crosspoint/studio/import.v4.json";
  HalFile metaFile;
  if (!Storage.exists(META) || !Storage.openFileForRead("STUDIO", META, metaFile)) return;
  JsonDocument meta;
  const bool parsed = !deserializeJson(meta, metaFile);
  metaFile.close();
  HalFile file;
  if (!parsed || !Storage.openFileForRead("STUDIO", STREAM, file)) return;
  const size_t length = file.size();
  auto stream = makeUniqueNoThrow<uint8_t[]>(length);
  const bool loaded = stream && file.read(stream.get(), length) == static_cast<int>(length);
  file.close();
  Storage.remove(STREAM);
  Storage.remove(META);
  if (!loaded) return;
  const std::string task = meta["task"] | "", hash = meta["hash"] | "";
  const size_t size = meta["size"] | size_t(0), header = meta["header"] | size_t(0);
  const size_t stopAt = meta["stop_at"] | size_t(0);
  auto& receiver = StudioReceiver::instance();
  auto feed = [&](size_t from, size_t to) {
    for (size_t at = from; at < to;) {
      const size_t n = std::min<size_t>(509, to - at);
      if (!receiver.feed(stream.get() + at, n)) return false;
      at += n;
    }
    return true;
  };
  // Stream offset of frame `index`'s record (or the end) in the full stream.
  auto recordStart = [&](size_t index) {
    size_t at = header;
    while (at + 6 <= length) {
      const size_t frame = size_t(stream[at]) | size_t(stream[at + 1]) << 8;
      if (frame >= index) return at;
      at += 6 + (size_t(stream[at + 2]) | size_t(stream[at + 3]) << 8 | size_t(stream[at + 4]) << 16 |
                 size_t(stream[at + 5]) << 24);
    }
    return length;
  };
  auto resume = StudioReceiver::resumeFor(header, size, StudioFrame::instance().resumeOffset(task, hash, size));
  bool ok = receiver.start(task, hash, 0, size, header, resume);
  LOG_INF("STICK", "v4 import: start %s, resume offset %u frames %u, need %s (%u)", ok ? "ok" : "failed",
          (unsigned)resume.offset, (unsigned)resume.framesDone, receiver.needHex().c_str(),
          (unsigned)receiver.needCount());
  const size_t first = resume.framesDone ? recordStart(resume.framesDone) : resume.offset;
  if (ok && stopAt && stopAt < length) {
    ok = feed(first, stopAt);
    receiver.abort(false);  // the link dropped: keep the partial
    resume = StudioReceiver::resumeFor(header, size, StudioFrame::instance().resumeOffset(task, hash, size));
    LOG_INF("STICK", "v4 import: stopped at %u, partial %u bytes -> resume offset %u frames %u", (unsigned)stopAt,
            (unsigned)StudioFrame::instance().resumeOffset(task, hash, size), (unsigned)resume.offset,
            (unsigned)resume.framesDone);
    ok = ok && receiver.start(task, hash, 0, size, header, resume);
    LOG_INF("STICK", "v4 import: resumed, need %s (%u)", receiver.needHex().c_str(), (unsigned)receiver.needCount());
    ok = ok && feed(resume.headerDone ? recordStart(resume.framesDone) : 0, length);
  } else if (ok) {
    ok = feed(first, length);
  }
  ok = ok && receiver.commit();
  if (!ok) receiver.abort(true);
  LOG_INF("STICK", "v4 import %s: %u stream bytes for %u bytes (%s)", ok ? "installed" : "failed", (unsigned)length,
          (unsigned)size, studio_v4::errorName(receiver.error()));
}
#endif

// STATE characteristic (≤ 512 bytes): firmware identity and capabilities,
// metrics, clock/trading-day state, the last phone sync, pending events (as
// many as fit, oldest first; `pending` is the full count) and an unreported
// OTA outcome. The phone uploads it to the cloud and acknowledges with `sync`.
std::string ProjectStickService::phoneStateJson() {
  JsonDocument doc;
  doc["v"] = 1;
  describeFirmware(doc);
  describeMetrics(doc["metrics"].to<JsonObject>());
  doc["clock"] = trustedUnixNow(serverTime) > 0;
  std::vector<ProjectStickEvent> events;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    const auto& s = PROJECT_STICK_STORE;
    doc["bound"] = s.bound;
    doc["cred"] = s.deviceToken.empty() ? 0 : 1;  // 2.7.2: a bound device may lack its cloud token
    doc["trading"] = s.tradingDay;
    doc["synced"] = s.lastPhoneSyncUtc;
    doc["pending"] = s.pendingEvents.size();
    events = s.pendingEvents;
  }
  if (ota_trial::hasPendingOutcome()) {
    const auto outcome = ota_trial::pendingOutcome();
    if (outcome.pending) {
      JsonObject out = doc["ota_outcome"].to<JsonObject>();
      out["version"] = outcome.version;
      out["rolled_back"] = outcome.rolledBack;
      out["reason"] = outcome.reason;
    }
  }
  JsonArray list = doc["events"].to<JsonArray>();
  for (const auto& event : events) {
    JsonObject obj = list.add<JsonObject>();
    obj["id"] = event.id;
    obj["type"] = event.type;
    if (!event.clientTs.empty()) obj["ts"] = event.clientTs;
    if (!event.studioTask.empty()) {
      obj["task"] = event.studioTask;
      obj["card"] = event.studioCard;
    }
    if (!event.detail.empty()) obj["detail"] = event.detail;
    if (measureJson(doc) > ble_setup::STATUS_LIMIT) {
      list.remove(list.size() - 1);
      break;
    }
  }
  std::string json;
  serializeJson(doc, json);
  return json;
}

void ProjectStickService::applyPhoneSync(const studio_ble::SyncRequest& request) {
  // The phone hands over the server clock it just received; a device without
  // a trusted clock adopts it, one with a clock only corrects a drift > 30 s.
  if (request.time >= 1735689600 && request.time < 4102444800LL) {
    adoptSystemTime(request.time);
    const std::time_t utc = static_cast<std::time_t>(request.time);
    std::tm parts{};
    project_stick::ShanghaiTime parsed;
    if (gmtime_r(&utc, &parts) &&
        project_stick::shanghaiFromUtc(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                                       parts.tm_min, parts.tm_sec, parsed)) {
      serverTime = parsed;
      serverTimeCapturedMs = millis();
    }
  }
  const int64_t syncedAt = request.time > 0 ? request.time : trustedUnixNow(serverTime);
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    auto& s = PROJECT_STICK_STORE;
    if (request.trading >= 0) s.tradingDay = request.trading == 1;
    for (const auto& id : request.ack) {
      s.pendingEvents.erase(std::remove_if(s.pendingEvents.begin(), s.pendingEvents.end(),
                                           [&id](const ProjectStickEvent& event) { return event.id == id; }),
                            s.pendingEvents.end());
    }
    if (syncedAt > 0) s.lastPhoneSyncUtc = syncedAt;
    s.saveToFile();
  }
  if (request.otaAck) ota_trial::clearOutcome();
  // A phone that completed an authenticated sync proves the image works.
  ota_trial::notePhoneSync();
  LOG_INF("STICK", "Phone sync applied (acked=%u trading=%d synced=%lld)", (unsigned)request.ack.size(),
          request.trading, (long long)syncedAt);
}

void ProjectStickService::unbindFromPhone() {
  LOG_INF("STICK", "Unbound by the phone over BLE");
  dropBinding();
}

bool ProjectStickService::phoneSyncFresh() const {
  const int64_t nowUtc = trustedUnixNow(serverTime);
  ProjectStickStateLock lock(projectStickStateMutex);
  return project_stick::phoneSyncFresh(nowUtc, PROJECT_STICK_STORE.lastPhoneSyncUtc);
}

void ProjectStickService::sendStudioFeedback(const std::string& task, const std::string& card, bool useful) {
  ProjectStickStateLock lock(projectStickStateMutex);
  ProjectStickEvent event;
  event.id = makeUuid();
  event.type = useful ? "studio_useful" : "studio_next";
  event.studioTask = task;
  event.studioCard = card;
  event.clientTs = project_stick::formatIso8601Shanghai(now());
  PROJECT_STICK_STORE.enqueue(std::move(event));
  PROJECT_STICK_STORE.saveToFile();
}

void ProjectStickService::adoptOwner(const std::string& owner) {
  ProjectStickStateLock lock(projectStickStateMutex);
  if (PROJECT_STICK_STORE.ownerId == owner) return;
  StudioFrame::instance().clear();
  studio_ble::revoke();
  auto& s = PROJECT_STICK_STORE;
  s.ownerId = owner;
  s.alertUntil = {};
  s.pendingEvents.clear();
  s.seenAlertIds.clear();
  s.saveToFile();
  refreshOwnership();
}
void ProjectStickService::dropBinding() {
  ProjectStickStateLock lock(projectStickStateMutex);
  auto& s = PROJECT_STICK_STORE;
  StudioFrame::instance().clear();
  studio_ble::revoke();
  s.bound = false;
  s.deviceToken.clear();
  s.ownerId.clear();
  s.alertUntil = {};
  s.pendingEvents.clear();
  s.seenAlertIds.clear();
  s.saveToFile();
  refreshOwnership();
}
bool ProjectStickService::refreshOwnership() {
  ProjectStickStateLock lock(projectStickStateMutex);
  if (localOwner == PROJECT_STICK_STORE.ownerId) return false;
  localOwner = PROJECT_STICK_STORE.ownerId;
  currentDisplay = {};
  return true;
}
