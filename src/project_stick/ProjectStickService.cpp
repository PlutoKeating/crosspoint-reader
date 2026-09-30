#include "ProjectStickService.h"

#include <HalPowerManager.h>

#include "FirmwareInstall.h"
#include "FirmwareUpdateState.h"
#include "StudioBluetooth.h"
#include "StudioFrame.h"
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
#include <ProjectStickPollPolicy.h>
#include <SecureHttpClient.h>
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
// Bumped when a BLE bind replaces the device token/owner; pairing and
// registration responses started before the bump are dropped.
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

// Firmware identity and capabilities sent with pairing and registration.
void describeFirmware(JsonDocument& request) {
  request["firmware_version"] = CROSSPOINT_VERSION;
  request["firmware_build"] = firmware_install::runningBuild();
  JsonObject capabilities = request["capabilities"].to<JsonObject>();
  capabilities["protocol"] = 2;
  capabilities["studio"] = 1;
  capabilities["studio_program"] = 1;
  capabilities["studio_ota"] = 1;
  // OTA protocol 2: resumable firmware download (offset), image identity
  // checks, trial boot with automatic rollback and outcome reports.
  capabilities["ota"] = 2;
  capabilities["panel"] = gpio.deviceIsX3() ? "xteink_x3" : "xteink_x4";
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
constexpr size_t IO_CHUNK = 1024;
// Vercel cold starts can take longer than five seconds before response headers.
// Keep POSTs below the user's 60-second UI patience budget while leaving enough
// room for the real X3 wolfSSL handshake and a cold API invocation.
constexpr uint32_t HTTP_TIMEOUT_MS = 20000;

class HttpBurst {
 public:
  explicit HttpBurst(freeink::SecureHttpClient& client) : client(client) {}
  ~HttpBurst() { client.end(); }

 private:
  freeink::SecureHttpClient& client;
};

// Studio polls run every few seconds; re-doing the TLS handshake each time
// costs about a second of CPU and radio time on the C3. Keep the connection
// between polls while the heap can afford the TLS session; the sync worker
// closes it after it has been idle for a while.
constexpr uint32_t KEEP_ALIVE_MIN_FREE_HEAP = 80 * 1024;
constexpr uint32_t KEEP_ALIVE_MIN_MAX_ALLOC = 32 * 1024;

class HttpKeepWarm {
 public:
  explicit HttpKeepWarm(freeink::SecureHttpClient& client) : client(client) {}
  ~HttpKeepWarm() {
#ifndef SIMULATOR
    if (ESP.getFreeHeap() >= KEEP_ALIVE_MIN_FREE_HEAP && ESP.getMaxAllocHeap() >= KEEP_ALIVE_MIN_MAX_ALLOC) return;
#endif
    client.end();
  }

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
#endif
  ProjectStickStateLock lock(projectStickStateMutex);
  if (!projectStickStoreInitialized) {
    PROJECT_STICK_STORE.loadFromFile();
    if (ensureIdentity()) PROJECT_STICK_STORE.saveToFile();
    projectStickStoreInitialized = true;
  }
  localOwner = PROJECT_STICK_STORE.ownerId;
  StudioFrame::instance().load();
  currentDisplay.alertUntil = PROJECT_STICK_STORE.alertUntil;
}

bool ProjectStickService::ensureIdentity() {
  if (!PROJECT_STICK_STORE.deviceId.empty()) return false;
  PROJECT_STICK_STORE.deviceId = makeUuid();
  LOG_INF("STICK", "Created device identity %s", PROJECT_STICK_STORE.deviceId.c_str());
  return true;
}

ProjectStickService::SyncReport ProjectStickService::sync(bool registerFirst) {
  HttpBurst burst(http);
  const uint32_t syncStartedMs = millis();
  SyncReport report;
  inactive = false;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    registerFirst = registerFirst || !PROJECT_STICK_STORE.bound;
  }
  if (registerFirst) {
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
    if (ota_trial::hasPendingOutcome()) reportFirmwareOutcome();
  }

  {
    ProjectStickStateLock lock(projectStickStateMutex);
    if (!PROJECT_STICK_STORE.bound) {
      report.result = SyncResult::Unbound;
      return report;
    }
  }

  syncStudio();
  flushEvents();
  report.result = SyncResult::Synced;
  LOG_INF("STICK", "Sync finished (total=%ums)", (unsigned)(millis() - syncStartedMs));
  return report;
}

bool ProjectStickService::ensurePairing(int& status) {
  std::string deviceId;
  std::string deviceToken;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceId = PROJECT_STICK_STORE.deviceId;
    deviceToken = PROJECT_STICK_STORE.deviceToken;
  }
  const uint32_t generation = bleBindingGeneration.load();
  JsonDocument request;
  request["device_id"] = deviceId;
  describeFirmware(request);
  std::string body;
  serializeJson(request, body);
  std::string response;
  if (!requestPost("/api/v2/device/pairing", body, response, status) || status != 200) return false;
  if (generation != bleBindingGeneration.load()) {
    status = 0;
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  const std::string returnedToken = doc["device_token"] | "";
  const std::string returnedCode = doc["pairing_code"] | "";
  if (returnedCode.size() != 8 || (deviceToken.empty() && returnedToken.empty())) return false;
  ProjectStickStateLock lock(projectStickStateMutex);
  if (generation != bleBindingGeneration.load() || PROJECT_STICK_STORE.bound) return false;
  if (!returnedToken.empty()) PROJECT_STICK_STORE.deviceToken = returnedToken.substr(0, 96);
  PROJECT_STICK_STORE.pairingCode = returnedCode.substr(0, 8);
  return PROJECT_STICK_STORE.saveToFile();
}

bool ProjectStickService::registerDevice(int& status) {
  LOG_INF("STICK", "Registering device (firmware=%s)", CROSSPOINT_VERSION);
  const uint32_t generation = bleBindingGeneration.load();
  bool bound = false;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    bound = PROJECT_STICK_STORE.bound;
  }
  if (!bound && !ensurePairing(status)) {
    if (status == 401) {
      // The server holds a different token for this unbound identity (SD state
      // restored or copied, or a token lost mid-pairing). Nothing is bound to
      // it, so start over with a fresh identity instead of failing forever.
      ProjectStickStateLock lock(projectStickStateMutex);
      // A phone may have bound the device over BLE meanwhile (new token).
      if (generation != bleBindingGeneration.load() || PROJECT_STICK_STORE.bound) return false;
      LOG_ERR("STICK", "Unbound identity %s was rejected; creating a new one", PROJECT_STICK_STORE.deviceId.c_str());
      PROJECT_STICK_STORE.deviceId = makeUuid();
      PROJECT_STICK_STORE.deviceToken.clear();
      PROJECT_STICK_STORE.pairingCode.clear();
      PROJECT_STICK_STORE.saveToFile();
      return false;
    }
    if (!project_stick::pairingFailureAllowsRegistration(status)) {
      LOG_ERR("STICK", "Device pairing refresh failed (status=%d)", status);
      return false;
    }
    // The claim may already be committed on the server while this device
    // still has bound=false on SD. Continue with the authenticated register
    // request so the server can return the authoritative bound state.
    LOG_INF("STICK", "Pairing already claimed; continuing registration");
  }
  JsonDocument request;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    request["device_id"] = PROJECT_STICK_STORE.deviceId;
  }
  describeFirmware(request);
  std::string body;
  serializeJson(request, body);

  std::string response;
  if (!requestPost("/api/v2/device/register", body, response, status) || status != 200) {
    LOG_ERR("STICK", "Device registration failed (status=%d)", status);
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
    if (!doc["studio_poll_seconds"].isNull())
      PROJECT_STICK_STORE.studioPollSeconds = project_stick::clampStudioPollSeconds(doc["studio_poll_seconds"] | int64_t(0));
    PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | false;
    adoptOwner(doc["owner_id"] | "");
    PROJECT_STICK_STORE.bound = doc["bound"] | false;
    if (PROJECT_STICK_STORE.bound)
      PROJECT_STICK_STORE.pairingCode.clear();
    else {
      StudioFrame::instance().clear();
      studio_ble::revoke();
    }
    parseServerTime(doc["server_time"] | "");
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

bool ProjectStickService::pollAlerts() {
  project_stick::ShanghaiTime current;
  bool eligible = false;
  std::string deviceId;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    current = now();
    eligible = PROJECT_STICK_STORE.tradingDay && current.valid;
    deviceId = PROJECT_STICK_STORE.deviceId;
  }
  const uint16_t minute = current.minuteOfDay();
  eligible = eligible && ((minute >= 570 && minute < 690) || (minute >= 780 && minute < 900));
#ifdef SIMULATOR
  eligible = eligible || std::getenv("CROSSPOINT_SIM_FORCE_ALERT_WINDOW") != nullptr;
#endif
  if (!eligible) return false;

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
  PROJECT_STICK_STORE.saveToFile();
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
  if (!trustedClockReady()) return false;
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
    std::string deviceToken;
    {
      ProjectStickStateLock lock(projectStickStateMutex);
      deviceToken = PROJECT_STICK_STORE.deviceToken;
    }
    if (!deviceToken.empty() && path.rfind("/api/v2/device/", 0) == 0) {
      http.addHeader("Authorization", "Bearer " + deviceToken);
    }
    status = http.sendRequest("POST", body);
    ota_trial::noteApiResult(status, WiFi.status() == WL_CONNECTED);
    response = http.getString();
#ifdef SIMULATOR
    const bool complete = true;
#else
    const bool complete = http.responseComplete();
#endif
    if (status > 0 && complete) {
      noteApiResult(status, retryAfterHeader(http));
      LOG_INF("STICK", "POST %s completed (status=%d bytes=%u time=%ums)", path.c_str(), status,
              (unsigned)response.size(), (unsigned)(millis() - attemptStartedMs));
      // 429 and 5xx are answers too, but callers treat them as failures.
      return status != 429 && status < 500;
    }
    LOG_ERR("STICK", "POST %s failed (status=%d complete=%u bytes=%u)", path.c_str(), status, complete ? 1u : 0u,
            (unsigned)response.size());
    if (attempt == 0) delay(500);
  }
  noteApiResult(status > 0 ? status : -1, "");
  return false;
}

bool ProjectStickService::fetchJson(const std::string& url, std::string& response, size_t maxBytes) {
  // Same policy as requestPost: one retry, for transport failures only.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    response.clear();
    response.reserve(std::min<size_t>(maxBytes, 4096));
    const bool ok = fetchAuthenticated(url, [&response, maxBytes](const uint8_t* data, size_t length) {
      if (length > maxBytes - response.size()) return false;
      response.append(reinterpret_cast<const char*>(data), length);
      return true;
    });
    if (ok) return true;
    if (lastFetchStatus > 0) break;
    if (attempt == 0) delay(500);
  }
  response.clear();
  return false;
}


bool ProjectStickService::fetchAuthenticated(const std::string& url,
                                             const std::function<bool(const uint8_t*, size_t)>& onData) {
  std::string deviceToken;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceToken = PROJECT_STICK_STORE.deviceToken;
  }
  lastFetchStatus = 0;
  if (!trustedClockReady()) return false;
  if (apiBlockedNow()) {
    lastFetchStatus = 429;  // not a transport failure: do not retry
    return false;
  }
  if (!http.begin(url)) return false;
  // Same pinned-root client with or without a credential; the ESP-IDF HTTP
  // client (and the mbedTLS TLS stack it pulls in) is not linked.
  if (!deviceToken.empty()) http.addHeader("Authorization", "Bearer " + deviceToken);
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
  noteApiResult(lastFetchStatus, retryAfterHeader(http));
  return success;
}




bool ProjectStickService::parseServerTime(const char* value) {
  project_stick::ShanghaiTime parsed;
  if (!project_stick::parseIso8601ToShanghai(value, parsed)) return false;
  serverTime = parsed;
  serverTimeCapturedMs = millis();
  return true;
}

project_stick::ShanghaiTime ProjectStickService::now() const {
  if (serverTime.valid) return project_stick::advanceTime(serverTime, (millis() - serverTimeCapturedMs) / 1000);
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
#ifdef SIMULATOR
  const std::time_t wallTime = std::time(nullptr);
  std::tm utcTime{};
  if (wallTime <= 0 || !gmtime_r(&wallTime, &utcTime)) return {};
  year = static_cast<uint16_t>(utcTime.tm_year + 1900);
  month = static_cast<uint8_t>(utcTime.tm_mon + 1);
  day = static_cast<uint8_t>(utcTime.tm_mday);
  hour = static_cast<uint8_t>(utcTime.tm_hour);
  minute = static_cast<uint8_t>(utcTime.tm_min);
  second = static_cast<uint8_t>(utcTime.tm_sec);
#else
  if (!halClock.getDateTime(year, month, day, hour, minute, second)) {
    const std::time_t wall = std::time(nullptr);
    std::tm utc{};
    if (wall < 1735689600 || !gmtime_r(&wall, &utc)) return {};
    year = utc.tm_year + 1900;
    month = utc.tm_mon + 1;
    day = utc.tm_mday;
    hour = utc.tm_hour;
    minute = utc.tm_min;
    second = utc.tm_sec;
  }
#endif
  char utc[32];
  snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", year, month, day, hour, minute, second);
  project_stick::ShanghaiTime rtcTime;
  project_stick::parseIso8601ToShanghai(utc, rtcTime);
  return rtcTime;
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
uint32_t ProjectStickService::studioPollSeconds() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.studioPollSeconds;
}
bool ProjectStickService::apiBlocked() { return apiBlockedNow(); }
bool ProjectStickService::apiRateLimited() {
  std::lock_guard<std::mutex> lock(apiBackoffMutex);
  return apiBackoff.rateLimited(millis());
}
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

std::string ProjectStickService::pairingCode() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.pairingCode;
}

std::string ProjectStickService::deviceId() const {
  ProjectStickStateLock lock(projectStickStateMutex);
  return PROJECT_STICK_STORE.deviceId;
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
  s.pairingCode.clear();
  const bool saved = s.saveToFile();
  LOG_INF("STICK", "Bound over BLE (owner=%s saved=%u)", owner.c_str(), saved ? 1u : 0u);
  return saved;
}


void ProjectStickService::closeIdleConnection() { http.end(); }

void ProjectStickService::syncStudio() {
  HttpKeepWarm connection(http);
  if (!isBound() || studio_ble::connected()) return;

  auto& frame = StudioFrame::instance();
  auto active = frame.snapshot();
  auto report = [&](const std::string& task, const char* state, const std::string& hash, const char* error = "") {
    JsonDocument doc;
    doc["device_id"] = PROJECT_STICK_STORE.deviceId;
    doc["task_id"] = task;
    doc["state"] = state;
    doc["card_id"] = frame.displaySnapshot().card;
    if (frame.snapshot().task != task) doc["override_task_id"] = frame.snapshot().task;
    doc["sha256"] = hash;
    doc["received_bytes"] = std::string(state) == "failed"         ? 0
                            : frame.displaySnapshot().task == task ? frame.displaySnapshot().size
                                                                   : frame.received();
    doc["error"] = error;
    std::string body, response;
    serializeJson(doc, body);
    int status = 0;
    return requestPost("/api/v2/device/studio", body, response, status) && status == 200;
  };
  const auto visual = frame.displaySnapshot();
  if (frame.needsReport() && visual.origin == "cloud" && report(visual.task, "displayed", visual.hash))
    frame.acknowledge(visual.task);
  std::string response;
  std::string url = baseUrl + "/api/v2/device/studio?device_id=" + PROJECT_STICK_STORE.deviceId;
#ifndef SIMULATOR
  url += "&heap_free=" + std::to_string(ESP.getFreeHeap()) + "&heap_min=" + std::to_string(ESP.getMinFreeHeap()) +
         "&sd_total=" + std::to_string(Storage.totalBytes()) + "&sd_used=" + std::to_string(Storage.usedBytes()) +
         "&battery_percent=" + std::to_string(powerManager.getBatteryPercentage()) +
         "&uptime_ms=" + std::to_string(millis()) + "&wifi_rssi=" + std::to_string(WiFi.RSSI());
#endif
  if (!fetchJson(url, response, 4096)) return;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return;
  adoptOwner(doc["owner_id"] | "");
  if (!doc["studio_poll_seconds"].isNull()) {
    ProjectStickStateLock lock(projectStickStateMutex);
    const uint32_t seconds = project_stick::clampStudioPollSeconds(doc["studio_poll_seconds"] | int64_t(0));
    if (PROJECT_STICK_STORE.studioPollSeconds != seconds) {
      PROJECT_STICK_STORE.studioPollSeconds = seconds;
      PROJECT_STICK_STORE.saveToFile();
    }
  }
  flushEvents();
  if (ota_trial::hasPendingOutcome()) reportFirmwareOutcome();
  // Servers with OTA protocol 2 say whether a command is queued; older ones
  // omit the flag and the device keeps polling /commands.
  const bool commandPending = doc["command_pending"].isNull() || doc["command_pending"].as<bool>();
  if (commandPending && syncStudioCommand()) return;
  if (!doc["bluetooth"].isNull())
    studio_ble::configure(PROJECT_STICK_STORE.deviceId, doc["bluetooth"]["secret"] | "", doc["bluetooth"]["epoch"] | 0,
                          doc["bluetooth"]["owner_id"] | "");
  frame.reconciled(doc["accepted_ble_task"] | "");
  active = frame.snapshot();
  if (doc["target"].isNull()) {
    if (active.origin == "cloud") frame.restore();
    return;
  }
  const std::string task = doc["target"]["task_id"] | "";
  const std::string hash = doc["target"]["sha256"] | "";
  const int64_t expires = doc["target"]["expires_epoch"] | int64_t(0);
  if (active.origin == "ble" || active.task == task || frame.busy()) return;
  if (studioAttemptTask != task) {
    studioAttemptTask = task;
    studioAttempts = 0;
  }
  if (studioAttempts >= 3 || (studioAttempts && static_cast<int32_t>(millis() - studioRetryAt) < 0)) return;
  ++studioAttempts;
  studioRetryAt = millis() + 15000;
  const size_t size = doc["target"]["size"] | size_t(0);
  if (!frame.start(task, hash, expires, "cloud", size)) {
    report(task, "failed", hash, "设备存储不足、忙碌或画面大小无效");
    return;
  }
  report(task, "transferring", hash);
  const std::string path = "/api/v2/device/studio/frame?device_id=" + PROJECT_STICK_STORE.deviceId +
                           "&task_id=" + task + "&offset=" + std::to_string(frame.received());
  const bool fetched =
      frame.received() == size || fetchAuthenticated(baseUrl + path, [&](const uint8_t* data, size_t length) {
        return frame.append(frame.received(), data, length);
      });
  if (!fetched) {
    frame.abort();
    report(task, "failed", hash, "画面下载失败");
    return;
  }
  report(task, "verifying", hash);
  if (!frame.commit()) {
    frame.abort();
    report(task, "failed", hash, "画面校验失败");
    return;
  }
  const auto clock = now();
  if (clock.valid) frame.tick(clock.day * 86400LL + clock.secondOfDay - 8 * 3600);
  report(task, size > StudioFrame::BYTES && frame.snapshot().card.empty() ? "scheduled" : "refreshing", hash);
}

namespace {
constexpr char FIRMWARE_TEMP[] = "/.crosspoint/studio/firmware.tmp";
constexpr char FIRMWARE_META[] = "/.crosspoint/studio/firmware.meta";
// Both OTA slots in partitions.csv are 0x640000 bytes.
constexpr size_t MAX_FIRMWARE_BYTES = 0x640000;
constexpr size_t MIN_FIRMWARE_BYTES = 100000;
constexpr uint8_t MIN_OTA_BATTERY_PERCENT = 30;
constexpr uint32_t FIRMWARE_RETRY_MS = 60000;

struct InstallContext {
  const char* commandId;
  const char* version;
};
}  // namespace

bool ProjectStickService::syncStudioCommand() {
#ifndef SIMULATOR
  // A new image is still proving itself; installing now would overwrite the
  // slot it rolls back to. The command stays queued until the trial ends.
  if (ota_trial::active()) return false;
  if (firmwareRetryAtMs != 0 && static_cast<int32_t>(millis() - firmwareRetryAtMs) < 0) return false;
  firmwareRetryAtMs = 0;
  std::string response;
  if (!fetchJson(baseUrl + "/api/v2/device/commands?device_id=" + PROJECT_STICK_STORE.deviceId, response, 2048))
    return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) || doc["command"].isNull()) return false;
  const std::string id = doc["command"]["id"] | "", hash = doc["command"]["sha256"] | "";
  const std::string version = doc["command"]["version"] | "";
  const size_t size = doc["command"]["bytes"] | size_t(0);
  auto report = [&](const char* state, const char* error = "", int progress = -1) {
    JsonDocument body;
    body["device_id"] = PROJECT_STICK_STORE.deviceId;
    body["id"] = id;
    body["state"] = state;
    body["error"] = error;
    if (progress >= 0) body["progress"] = progress;
    std::string json, result;
    serializeJson(body, json);
    int status = 0;
    return requestPost("/api/v2/device/commands", json, result, status) && status == 200;
  };
  auto fail = [&](const char* error) {
    firmware_update::fail(error);
    report("failed", error);
    studio_ble::pause(false);
    return true;
  };
  if (id.size() != 36 || !validSha256(hash) || version.empty() || version.size() > 32 || size < MIN_FIRMWARE_BYTES ||
      size > MAX_FIRMWARE_BYTES) {
    report("failed", "Invalid firmware metadata");
    return true;
  }
  if (powerManager.getBatteryPercentage() < MIN_OTA_BATTERY_PERCENT) {
    firmware_update::fail("low_battery");
    report("failed", "Battery below 30%; charge before retrying");
    return true;
  }
  if (StudioFrame::instance().busy() || studio_ble::connected()) return true;
  studio_ble::pause(true);
  HalPowerManager::Lock powerLock;
  firmware_update::begin(version.c_str(), size);
  if (!report("downloading", "", 0)) {
    firmware_update::reset();
    studio_ble::pause(false);
    return true;
  }
  const auto download = downloadFirmware(id, hash, size);
  if (download == DownloadResult::Fatal) return fail("Firmware download failed");
  if (download == DownloadResult::Retry) {
    // Transport trouble: keep the command in `downloading` and the partial
    // file, and resume on a later poll instead of failing the whole update.
    LOG_INF("OTA", "Firmware download interrupted; retrying in %us", (unsigned)(FIRMWARE_RETRY_MS / 1000));
    firmwareRetryAtMs = millis() + FIRMWARE_RETRY_MS;
    firmware_update::reset();
    studio_ble::pause(false);
    return true;
  }

  firmware_update::setPhase(firmware_update::Phase::Verifying);
  report("verifying");
  std::string downloadedHash;
  HalFile file;
  const bool sized = Storage.openFileForRead("STUDIO", FIRMWARE_TEMP, file) && file.size() == size;
  file.close();
  if (!sized || !hashFile(FIRMWARE_TEMP, downloadedHash) || downloadedHash != hash) {
    Storage.remove(FIRMWARE_TEMP);
    Storage.remove(FIRMWARE_META);
    return fail("Firmware checksum mismatch");
  }
  const auto candidate = firmware_install::inspect(FIRMWARE_TEMP, version.c_str());
  if (!candidate.ok()) {
    Storage.remove(FIRMWARE_TEMP);
    Storage.remove(FIRMWARE_META);
    return fail(stick_fw::installVerdictName(candidate.verdict));
  }
  // The server re-checks here that the catalogue entry was not withdrawn.
  if (!report("installing")) {
    firmware_update::fail("Install not confirmed by server");
    studio_ble::pause(false);
    return true;
  }
  firmware_update::setPhase(firmware_update::Phase::Installing);
  InstallContext context{id.c_str(), version.c_str()};
  auto onProgress = +[](size_t written, size_t, void*) { firmware_update::setProgress(written); };
  auto armTrial = +[](const esp_partition_t* dest, void* ctx) {
    const auto* install = static_cast<const InstallContext*>(ctx);
    return ota_trial::arm(dest, install->commandId, install->version);
  };
  const auto result = firmware_flash::flashFromSdPath(FIRMWARE_TEMP, onProgress, &context, false, armTrial);
  if (result != firmware_flash::Result::OK) {
    ota_trial::disarm();  // the new image will not boot, so there is no trial
    return fail(firmware_flash::resultName(result));
  }
  Storage.remove(FIRMWARE_TEMP);
  Storage.remove(FIRMWARE_META);
  firmware_update::setPhase(firmware_update::Phase::Restarting);
  report("restarting");
  delay(1500);  // lets the UI paint the restarting state
  ESP.restart();
  return true;
#else
  return false;
#endif
}

namespace {
size_t fileSize(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) return 0;
  const size_t size = file.size();
  file.close();
  return size;
}
}  // namespace

ProjectStickService::DownloadResult ProjectStickService::downloadFirmware(const std::string& id,
                                                                          const std::string& hash, size_t size) {
  // A partial download survives errors, power loss, reboots and re-issued
  // commands: it is keyed on the image (hash + size), which every command for
  // the same catalogue entry serves byte-identically.
  const std::string meta = hash + " " + std::to_string(size);
  {
    HalFile metaFile;
    char stored[128] = {};
    if (Storage.openFileForRead("STUDIO", FIRMWARE_META, metaFile)) {
      const int read = metaFile.read(stored, sizeof(stored) - 1);
      stored[read > 0 ? read : 0] = '\0';
    }
    metaFile.close();
    if (meta != stored || fileSize(FIRMWARE_TEMP) > size) {
      Storage.remove(FIRMWARE_TEMP);
      Storage.mkdir("/.crosspoint/studio");
      if (!Storage.openFileForWrite("STUDIO", FIRMWARE_META, metaFile) ||
          metaFile.write(reinterpret_cast<const uint8_t*>(meta.data()), meta.size()) != meta.size()) {
        return DownloadResult::Retry;
      }
      metaFile.close();
    }
  }

  std::string deviceToken;
  {
    ProjectStickStateLock lock(projectStickStateMutex);
    deviceToken = PROJECT_STICK_STORE.deviceToken;
  }
  constexpr size_t FLUSH_INTERVAL = 256 * 1024;  // bounds what power loss can discard
  for (uint8_t attempt = 0; attempt < 4; ++attempt) {
    // The file on SD is the source of truth: a short write or an aborted pass
    // may have stored more (or fewer) bytes than the last callback counted.
    size_t offset = fileSize(FIRMWARE_TEMP);
    if (offset == size) return DownloadResult::Complete;
    if (offset > size) {
      Storage.remove(FIRMWARE_TEMP);
      offset = 0;
    }
    firmware_update::setProgress(offset);
    if (offset > 0) LOG_INF("OTA", "Resuming firmware download at %u/%u", (unsigned)offset, (unsigned)size);
    if (!trustedClockReady()) return DownloadResult::Retry;
    std::string url =
        baseUrl + "/api/v2/device/firmware?device_id=" + PROJECT_STICK_STORE.deviceId + "&command_id=" + id;
    if (offset > 0) url += "&offset=" + std::to_string(offset);
    HalFile output = Storage.open(FIRMWARE_TEMP, O_WRONLY | O_CREAT | O_APPEND);
    if (!output) return DownloadResult::Retry;
    if (!http.begin(url)) return DownloadResult::Fatal;
    http.addHeader("Authorization", "Bearer " + deviceToken);
    bool restartFromZero = false;
    const size_t requested = offset;
    size_t unflushed = 0;
    const int status = http.GET([&](const uint8_t* data, size_t length) {
      const int code = http.getStatus();
      // A server that ignores `offset` answers 200 with the whole image.
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
      LOG_INF("OTA", "Server does not support resume; restarting download");
      Storage.remove(FIRMWARE_TEMP);
      continue;
    }
    // Withdrawn, cancelled or not this device's command: resuming cannot help.
    if (status == 401 || status == 403 || status == 404 || status == 409 || status == 416) {
      return DownloadResult::Fatal;
    }
    LOG_INF("OTA", "Firmware download pass: status=%d stored %u/%u", status, (unsigned)fileSize(FIRMWARE_TEMP),
            (unsigned)size);
    if (attempt < 3) delay(1000UL << attempt);
  }
  return fileSize(FIRMWARE_TEMP) == size ? DownloadResult::Complete : DownloadResult::Retry;
}

ProjectStickService::FirmwareOffer ProjectStickService::checkFirmware() {
  HttpBurst burst(http);
  FirmwareOffer offer;
  std::string response;
  if (!fetchJson(
          baseUrl + "/api/v2/device/firmware/latest?device_id=" + PROJECT_STICK_STORE.deviceId + "&channel=stable",
          response, 4096)) {
    return offer;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) return offer;
  if (!(doc["bound"] | false)) {
    offer.status = FirmwareOffer::Status::Unbound;
  } else if (doc["latest"].isNull()) {
    offer.status = FirmwareOffer::Status::UpToDate;
  } else {
    offer.status = FirmwareOffer::Status::UpdateAvailable;
    offer.id = doc["latest"]["id"] | "";
    offer.version = doc["latest"]["version"] | "";
    offer.notes = doc["latest"]["notes"] | "";
    if (offer.id.size() != 36 || offer.version.empty()) offer.status = FirmwareOffer::Status::Failed;
  }
  return offer;
}

ProjectStickService::FirmwareOffer ProjectStickService::requestFirmware(const std::string& firmwareId) {
  HttpBurst burst(http);
  FirmwareOffer offer;
  offer.id = firmwareId;
  offer.status = FirmwareOffer::Status::RequestFailed;
  JsonDocument body;
  body["device_id"] = PROJECT_STICK_STORE.deviceId;
  body["action"] = "request";
  body["firmware_id"] = firmwareId;
  std::string json, response;
  serializeJson(body, json);
  int status = 0;
  if (!requestPost("/api/v2/device/commands", json, response, status) || status != 200) return offer;
  offer.status = FirmwareOffer::Status::Requested;
  // Install right away on this worker; progress is published for the UI.
  syncStudioCommand();
  return offer;
}

void ProjectStickService::reportFirmwareOutcome() {
  const auto outcome = ota_trial::pendingOutcome();
  if (!outcome.pending) return;
  // Confirmed installs complete on the server when the device reports the new
  // version; only rollbacks need an explicit failure report.
  if (!outcome.rolledBack || outcome.commandId[0] == '\0') {
    ota_trial::clearOutcome();
    return;
  }
  JsonDocument body;
  body["device_id"] = PROJECT_STICK_STORE.deviceId;
  body["id"] = outcome.commandId;
  body["state"] = "failed";
  char error[96];
  snprintf(error, sizeof(error), "Rolled back from %s (%s)", outcome.version, outcome.reason);
  body["error"] = error;
  std::string json, result;
  serializeJson(body, json);
  int status = 0;
  // 409 means the server already closed the command; either way it is settled.
  if (requestPost("/api/v2/device/commands", json, result, status) && (status == 200 || status == 409)) {
    ota_trial::clearOutcome();
  }
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
bool ProjectStickService::refreshOwnership() {
  ProjectStickStateLock lock(projectStickStateMutex);
  if (localOwner == PROJECT_STICK_STORE.ownerId) return false;
  localOwner = PROJECT_STICK_STORE.ownerId;
  currentDisplay = {};
  return true;
}
