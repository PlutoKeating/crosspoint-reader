#include "ProjectStickService.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <HalClock.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <utility>

#include "network/HttpDownloader.h"
#include "project_stick/ProjectStickStore.h"

#ifndef PROJECT_STICK_BASE_URL
#define PROJECT_STICK_BASE_URL "https://stockstick.vercel.app"
#endif

namespace {
constexpr size_t MAX_MANIFEST_FILES = 64;
constexpr size_t MAX_RELEASE_FILE_SIZE = 512 * 1024;
constexpr size_t MAX_MANIFEST_BYTES = 64 * 1024;
constexpr size_t MAX_ALERT_BYTES = 32 * 1024;
constexpr size_t MAX_SCHEDULE_BYTES = 32 * 1024;
constexpr size_t MAX_CONTENT_BYTES = 96 * 1024;
constexpr size_t IO_CHUNK = 1024;
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;

bool equalsIgnoreCase(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (tolower(static_cast<unsigned char>(left[i])) != tolower(static_cast<unsigned char>(right[i]))) return false;
  }
  return true;
}
}  // namespace

void ProjectStickService::begin() {
  baseUrl = PROJECT_STICK_BASE_URL;
  while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
  PROJECT_STICK_STORE.loadFromFile();
  if (ensureIdentity()) PROJECT_STICK_STORE.saveToFile();
}

bool ProjectStickService::ensureIdentity() {
  if (!PROJECT_STICK_STORE.deviceId.empty()) return false;
  PROJECT_STICK_STORE.deviceId = makeUuid();
  LOG_INF("STICK", "Created device identity %s", PROJECT_STICK_STORE.deviceId.c_str());
  return true;
}

ProjectStickService::SyncResult ProjectStickService::sync(bool registerFirst) {
  inactive = false;
  if (registerFirst) {
    int status = 0;
    if (!registerDevice(status)) {
      if (status == 403) {
        inactive = true;
        return SyncResult::Inactive;
      }
      return refreshScheduledContent() ? SyncResult::OfflineCache : SyncResult::Failed;
    }
  }

  flushEvents();
  const SyncResult result = syncManifest();
  if (result == SyncResult::Updated ||
      ((result == SyncResult::Unchanged || result == SyncResult::OfflineCache) && currentDisplay.copyId == 0)) {
    if (!refreshScheduledContent()) return SyncResult::NoContent;
  }
  return result;
}

bool ProjectStickService::registerDevice(int& status) {
  JsonDocument request;
  request["device_id"] = PROJECT_STICK_STORE.deviceId;
  request["firmware_version"] = CROSSPOINT_VERSION;
  std::string body;
  serializeJson(request, body);

  std::string response;
  if (!requestPost("/api/v1/device/register", body, response, status) || status != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;

  PROJECT_STICK_STORE.pollIntervalSeconds =
      std::clamp<uint32_t>(doc["poll_interval_seconds"] | 300, 30, 86400);
  PROJECT_STICK_STORE.alertPollIntervalSeconds =
      std::clamp<uint32_t>(doc["alert_poll_interval_seconds"] | 30, 10, 3600);
  PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | false;
  parseServerTime(doc["server_time"] | "");
  return PROJECT_STICK_STORE.saveToFile();
}

ProjectStickService::SyncResult ProjectStickService::syncManifest() {
  const std::string path = "/api/v1/device/manifest?device_id=" + PROJECT_STICK_STORE.deviceId +
                           "&current_version=" + std::to_string(PROJECT_STICK_STORE.activeVersion);
  std::string response;
  if (!fetchJson(baseUrl + path, response, MAX_MANIFEST_BYTES)) return SyncResult::OfflineCache;

  JsonDocument doc;
  if (deserializeJson(doc, response)) return SyncResult::Failed;
  parseServerTime(doc["server_time"] | "");
  if (doc["unchanged"] | false) return SyncResult::Unchanged;

  const uint32_t version = doc["version"] | 0;
  if (version == 0) return SyncResult::NoContent;
  JsonArrayConst fileArray = doc["files"].as<JsonArrayConst>();
  if (fileArray.isNull() || fileArray.size() == 0 || fileArray.size() > MAX_MANIFEST_FILES) return SyncResult::Failed;

  std::vector<ManifestFile> files;
  files.reserve(fileArray.size());
  for (JsonObjectConst item : fileArray) {
    ManifestFile file;
    file.path = item["path"] | "";
    file.sha256 = item["sha256"] | "";
    file.size = item["size"] | 0;
    if (!project_stick::isSafeReleasePath(file.path) || file.sha256.size() != 64 || file.size == 0 ||
        file.size > MAX_RELEASE_FILE_SIZE) {
      LOG_ERR("STICK", "Rejected manifest entry: %s", file.path.c_str());
      return SyncResult::Failed;
    }
    files.push_back(std::move(file));
  }

  if (!materializeRelease(version, files)) return SyncResult::Failed;
  PROJECT_STICK_STORE.activeVersion = version;
  scheduleCache.clear();
  if (!PROJECT_STICK_STORE.saveToFile()) return SyncResult::Failed;
  queueEvent("sync_completed", "", 0);
  flushEvents();
  return SyncResult::Updated;
}

bool ProjectStickService::materializeRelease(uint32_t version, const std::vector<ManifestFile>& files) {
  if (!Storage.mkdir(releaseRoot(version).c_str(), true) && !Storage.exists(releaseRoot(version).c_str())) return false;

  for (const auto& file : files) {
    const std::string destination = releaseFile(version, file.path);
    if (!ensureParentDirectory(destination)) return false;

    bool ready = false;
    if (PROJECT_STICK_STORE.activeVersion != 0) {
      const std::string previous = releaseFile(PROJECT_STICK_STORE.activeVersion, file.path);
      std::string oldHash;
      if (Storage.exists(previous.c_str()) && hashFile(previous, oldHash) && equalsIgnoreCase(oldHash, file.sha256)) {
        ready = copyFile(previous, destination);
      }
    }

    if (!ready) {
      const std::string url = baseUrl + "/api/v1/device/releases/" + std::to_string(version) + "/" + file.path +
                              "?device_id=" + PROJECT_STICK_STORE.deviceId;
      for (uint8_t attempt = 0; attempt < 3 && !ready; ++attempt) {
        ready = HttpDownloader::downloadToFile(url, destination) == HttpDownloader::OK;
        if (!ready && attempt < 2) delay(250UL << attempt);
      }
      if (!ready) return false;
    }

    if (!fileWithinLimit(destination, file.size) || !fileWithinLimit(destination, MAX_RELEASE_FILE_SIZE)) {
      LOG_ERR("STICK", "Downloaded file exceeds manifest size: %s", file.path.c_str());
      Storage.remove(destination.c_str());
      return false;
    }
    std::string downloadedHash;
    if (!hashFile(destination, downloadedHash) || !equalsIgnoreCase(downloadedHash, file.sha256)) {
      LOG_ERR("STICK", "SHA-256 mismatch: %s", file.path.c_str());
      Storage.remove(destination.c_str());
      return false;
    }
  }
  return true;
}

bool ProjectStickService::loadSchedule(std::vector<project_stick::ScheduleWindow>& windows) {
  const std::string path = releaseFile(PROJECT_STICK_STORE.activeVersion, "schedule.json");
  if (!fileWithinLimit(path, MAX_SCHEDULE_BYTES)) return false;
  const String body = Storage.readFile(path.c_str());
  if (body.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArrayConst array = doc["windows"].as<JsonArrayConst>();
  windows.clear();
  windows.reserve(std::min<size_t>(array.size(), 32));
  for (JsonObjectConst item : array) {
    if (windows.size() >= 32) break;
    project_stick::ScheduleWindow window;
    window.scenario = item["scenario"] | "";
    window.tradingDayOnly = item["trading_day_only"] | false;
    window.allDay = item["all_day"] | false;
    window.enabled = item["enabled"] | true;
    if (window.scenario.empty()) continue;
    if (!window.allDay &&
        (!project_stick::parseClockMinute(item["start"] | "", window.startMinute) ||
         !project_stick::parseClockMinute(item["end"] | "", window.endMinute))) {
      continue;
    }
    windows.push_back(std::move(window));
  }
  return !windows.empty();
}

bool ProjectStickService::loadContent(const std::string& scenario,
                                      std::vector<project_stick::ContentCopy>& copies) {
  if (!project_stick::isSafeReleasePath("content/" + scenario + ".json")) return false;
  const std::string path = releaseFile(PROJECT_STICK_STORE.activeVersion, "content/" + scenario + ".json");
  if (!fileWithinLimit(path, MAX_CONTENT_BYTES)) return false;
  const String body = Storage.readFile(path.c_str());
  if (body.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArrayConst array = doc["copies"].as<JsonArrayConst>();
  copies.clear();
  copies.reserve(std::min<size_t>(array.size(), 64));
  for (JsonObjectConst item : array) {
    if (copies.size() >= 64) break;
    project_stick::ContentCopy copy;
    copy.id = item["id"] | 0;
    copy.text = item["text"] | "";
    copy.tone = item["tone"] | "";
    copy.weight = std::clamp<uint16_t>(item["weight"] | 1, 1, 1000);
    if (copy.id != 0 && !copy.text.empty()) copies.push_back(std::move(copy));
  }
  return !copies.empty();
}

bool ProjectStickService::refreshScheduledContent(const char* forcedScenario) {
  if (PROJECT_STICK_STORE.activeVersion == 0) return false;
  const project_stick::ShanghaiTime current = now();
  std::string scenario;
  if (forcedScenario && *forcedScenario) {
    scenario = forcedScenario;
  } else {
    if (scheduleCache.empty() && !loadSchedule(scheduleCache)) return false;
    const auto* selected =
        project_stick::selectSchedule(scheduleCache, current.valid ? current.minuteOfDay() : 0,
                                      PROJECT_STICK_STORE.tradingDay);
    if (!selected) return false;
    scenario = selected->scenario;
  }

  if (!loadContent(scenario, contentCache)) return false;
  if (current.valid && current.day != PROJECT_STICK_STORE.usedDay) {
    PROJECT_STICK_STORE.usedDay = current.day;
    PROJECT_STICK_STORE.usedCopyIds.clear();
  }
#ifdef SIMULATOR
  const uint32_t randomValue = static_cast<uint32_t>(std::rand());
#else
  const uint32_t randomValue = esp_random();
#endif
  const auto* selected = project_stick::selectCopy(contentCache, PROJECT_STICK_STORE.usedCopyIds, randomValue);
  if (!selected) return false;

  currentDisplay.scenario = scenario;
  currentDisplay.text = selected->text;
  currentDisplay.tone = selected->tone;
  currentDisplay.copyId = selected->id;
  currentDisplay.alert = forcedScenario && strcmp(forcedScenario, "volatility_alert") == 0;
  PROJECT_STICK_STORE.markCopyUsed(current.valid ? current.day : PROJECT_STICK_STORE.usedDay, selected->id);
  PROJECT_STICK_STORE.saveToFile();
  queueEvent("trigger_fired", scenario, selected->id);
  queueEvent("screen_view", scenario, selected->id);
  flushEvents();
  return true;
}

bool ProjectStickService::refreshIfScheduleChanged() {
  if (PROJECT_STICK_STORE.activeVersion == 0) return false;
  if (scheduleCache.empty() && !loadSchedule(scheduleCache)) return false;
  const project_stick::ShanghaiTime current = now();
  const auto* selected =
      project_stick::selectSchedule(scheduleCache, current.valid ? current.minuteOfDay() : 0,
                                    PROJECT_STICK_STORE.tradingDay);
  if (!selected || selected->scenario == currentDisplay.scenario) return false;
  return refreshScheduledContent(selected->scenario.c_str());
}

bool ProjectStickService::pollAlerts() {
  const project_stick::ShanghaiTime current = now();
  if (!PROJECT_STICK_STORE.tradingDay || !current.valid) return false;
  const uint16_t minute = current.minuteOfDay();
  if (!((minute >= 570 && minute < 690) || (minute >= 780 && minute < 900))) return false;

  std::string response;
  const std::string url =
      baseUrl + "/api/v1/device/alerts?device_id=" + PROJECT_STICK_STORE.deviceId;
  if (!fetchJson(url, response, MAX_ALERT_BYTES)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  parseServerTime(doc["server_time"] | "");
  PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | PROJECT_STICK_STORE.tradingDay;

  for (JsonObjectConst alert : doc["alerts"].as<JsonArrayConst>()) {
    const int64_t id = alert["id"] | 0;
    if (id != 0 && !PROJECT_STICK_STORE.hasSeenAlert(id)) {
      if (refreshScheduledContent(alert["scenario"] | "volatility_alert")) {
        PROJECT_STICK_STORE.markAlertSeen(id);
        PROJECT_STICK_STORE.saveToFile();
        return true;
      }
    }
  }
  PROJECT_STICK_STORE.saveToFile();
  return false;
}

void ProjectStickService::sendFeedback(bool useful) {
  if (currentDisplay.copyId == 0) return;
  queueEvent(useful ? "feedback_useful" : "feedback_meh", currentDisplay.scenario, currentDisplay.copyId);
  flushEvents();
}

void ProjectStickService::sendManualRefresh() {
  queueEvent("manual_refresh", currentDisplay.scenario, currentDisplay.copyId);
  if (currentDisplay.scenario.empty()) {
    refreshScheduledContent("manual_refresh");
  } else {
    const std::string scenario = currentDisplay.scenario;
    refreshScheduledContent(scenario.c_str());
  }
}

void ProjectStickService::queueEvent(const char* type, const std::string& scenario, int64_t copyId) {
  ProjectStickEvent event;
  event.id = makeUuid();
  event.type = type;
  event.scenario = scenario;
  event.copyId = copyId;
  event.clientTs = project_stick::formatIso8601Shanghai(now());
  PROJECT_STICK_STORE.enqueue(std::move(event));
  PROJECT_STICK_STORE.saveToFile();
}

bool ProjectStickService::flushEvents() {
  if (PROJECT_STICK_STORE.pendingEvents.empty()) return true;
  JsonDocument request;
  request["device_id"] = PROJECT_STICK_STORE.deviceId;
  JsonArray events = request["events"].to<JsonArray>();
  for (const auto& event : PROJECT_STICK_STORE.pendingEvents) {
    JsonObject obj = events.add<JsonObject>();
    obj["client_event_id"] = event.id;
    obj["event_type"] = event.type;
    if (!event.scenario.empty()) obj["scenario"] = event.scenario;
    if (event.copyId != 0) obj["copy_id"] = event.copyId;
    if (!event.clientTs.empty()) obj["client_ts"] = event.clientTs;
  }
  std::string body;
  serializeJson(request, body);
  std::string response;
  int status = 0;
  if (!requestPost("/api/v1/device/events", body, response, status) || status != 200) return false;
  PROJECT_STICK_STORE.pendingEvents.clear();
  return PROJECT_STICK_STORE.saveToFile();
}

bool ProjectStickService::requestPost(const std::string& path, const std::string& body, std::string& response,
                                      int& status) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
#ifndef SIMULATOR
    http.setUserAgent("Project.Stick-CrossPoint-" CROSSPOINT_VERSION);
    http.setFollowRedirects(3);
#endif
    if (!http.begin(baseUrl + path)) return false;
    http.addHeader("Content-Type", "application/json");
    status = http.sendRequest("POST", body);
    response = http.getString();
#ifdef SIMULATOR
    const bool complete = true;
#else
    const bool complete = http.responseComplete();
#endif
    http.end();
    if (status > 0 && complete && status < 500) return true;
    if (attempt < 2) delay(250UL << attempt);
  }
  return false;
}

bool ProjectStickService::fetchJson(const std::string& url, std::string& response, size_t maxBytes) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    response.clear();
    response.reserve(std::min<size_t>(maxBytes, 4096));
    const bool ok = HttpDownloader::fetchUrl(url, [&response, maxBytes](const uint8_t* data, size_t length) {
      if (length > maxBytes - response.size()) return false;
      response.append(reinterpret_cast<const char*>(data), length);
      return true;
    });
    if (ok) return true;
    if (attempt < 2) delay(250UL << attempt);
  }
  response.clear();
  return false;
}

bool ProjectStickService::fileWithinLimit(const std::string& path, size_t maxBytes) {
  HalFile file;
  if (!Storage.openFileForRead("STICK", path, file)) return false;
  return file.fileSize64() <= maxBytes;
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
  if (!halClock.getDateTime(year, month, day, hour, minute)) return {};
  char utc[32];
  snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:00Z", year, month, day, hour, minute);
  project_stick::ShanghaiTime rtcTime;
  project_stick::parseIso8601ToShanghai(utc, rtcTime);
  return rtcTime;
}

bool ProjectStickService::ensureParentDirectory(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string parent = path.substr(0, slash);
  return Storage.mkdir(parent.c_str(), true) || Storage.exists(parent.c_str());
}

bool ProjectStickService::copyFile(const std::string& source, const std::string& destination) {
  HalFile input;
  HalFile output;
  if (!Storage.openFileForRead("STICK", source, input) ||
      !Storage.openFileForWrite("STICK", destination, output)) {
    return false;
  }
  auto buffer = makeUniqueNoThrow<uint8_t[]>(IO_CHUNK);
  if (!buffer) {
    LOG_ERR("STICK", "OOM: copy buffer");
    return false;
  }
  while (input.available()) {
    const int count = input.read(buffer.get(), IO_CHUNK);
    if (count <= 0 || output.write(buffer.get(), static_cast<size_t>(count)) != static_cast<size_t>(count)) return false;
  }
  output.flush();
  return true;
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

std::string ProjectStickService::releaseRoot(uint32_t version) const {
  return "/.crosspoint/project_stick/releases/" + std::to_string(version);
}

std::string ProjectStickService::releaseFile(uint32_t version, const std::string& path) const {
  return releaseRoot(version) + "/" + path;
}

std::string ProjectStickService::makeUuid() {
  uint8_t bytes[16];
#ifdef SIMULATOR
  for (auto& byte : bytes) byte = static_cast<uint8_t>(std::rand());
#else
  esp_fill_random(bytes, sizeof(bytes));
#endif
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
  char value[37];
  snprintf(value, sizeof(value),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1],
           bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
  return value;
}

uint32_t ProjectStickService::activeVersion() const { return PROJECT_STICK_STORE.activeVersion; }
uint32_t ProjectStickService::pendingEventCount() const {
  return static_cast<uint32_t>(PROJECT_STICK_STORE.pendingEvents.size());
}
uint32_t ProjectStickService::pollIntervalSeconds() const { return PROJECT_STICK_STORE.pollIntervalSeconds; }
uint32_t ProjectStickService::alertPollIntervalSeconds() const {
  return PROJECT_STICK_STORE.alertPollIntervalSeconds;
}
bool ProjectStickService::isTradingDay() const { return PROJECT_STICK_STORE.tradingDay; }
