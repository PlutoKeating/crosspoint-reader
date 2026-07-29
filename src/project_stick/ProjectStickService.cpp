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
#ifdef SIMULATOR
#include <random>
#endif
#include <utility>

#include "network/HttpDownloader.h"
#include "project_stick/ProjectStickStore.h"

#ifndef PROJECT_STICK_BASE_URL
#define PROJECT_STICK_BASE_URL "https://stockstick.vercel.app"
#endif

namespace {
constexpr size_t MAX_RELEASE_FILE_SIZE = 512 * 1024;
constexpr size_t MAX_MANIFEST_BYTES = 64 * 1024;
constexpr size_t MAX_ALERT_BYTES = 32 * 1024;
constexpr size_t MAX_SCHEDULE_BYTES = 32 * 1024;
constexpr size_t MAX_CONTENT_BYTES = 96 * 1024;
constexpr size_t IO_CHUNK = 1024;
constexpr uint32_t HTTP_TIMEOUT_MS = 5000;
constexpr char DATA_ROOT[] = "/.crosspoint/project_stick";
constexpr char OBJECT_ROOT[] = "/.crosspoint/project_stick/objects";
constexpr char SNAPSHOT_ROOT[] = "/.crosspoint/project_stick/snapshots";
constexpr char MANIFEST_TEMP[] = "/.crosspoint/project_stick/manifest.tmp";
constexpr char SNAPSHOT_TEMP[] = "/.crosspoint/project_stick/snapshots/incoming.tmp";
constexpr size_t MAX_RELEASE_PATH = 192;
constexpr uint32_t PARSER_MIN_FREE_HEAP = 12 * 1024;
constexpr uint32_t PARSER_MIN_MAX_BLOCK = 4 * 1024;

bool equalsIgnoreCase(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (tolower(static_cast<unsigned char>(left[i])) != tolower(static_cast<unsigned char>(right[i]))) return false;
  }
  return true;
}

bool validSha256(const std::string& value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool hasParserHeap() {
#ifdef SIMULATOR
  return true;
#else
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  if (freeHeap >= PARSER_MIN_FREE_HEAP && maxBlock >= PARSER_MIN_MAX_BLOCK) return true;
  LOG_ERR("STICK", "Parser heap guard: free=%u max=%u", (unsigned)freeHeap, (unsigned)maxBlock);
  return false;
#endif
}

bool readSnapshotEntry(HalFile& file, project_stick::ReleaseFileEntry& entry) {
  entry = {};
  uint8_t field = 0;
  std::string sizeText;
  while (file.available()) {
    const int raw = file.read();
    if (raw < 0) return false;
    const char value = static_cast<char>(raw);
    if (value == '\n') break;
    if (value == '\t' && field < 2) {
      ++field;
      continue;
    }
    std::string* destination = field == 0 ? &entry.sha256 : (field == 1 ? &sizeText : &entry.path);
    const size_t limit = field == 0 ? 64 : (field == 1 ? 20 : MAX_RELEASE_PATH);
    if (destination->size() >= limit) return false;
    destination->push_back(value);
  }
  if (field != 2 || !validSha256(entry.sha256) || sizeText.empty() ||
      !project_stick::isSafeReleasePath(entry.path)) {
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(sizeText.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed == 0) return false;
  entry.size = static_cast<size_t>(parsed);
  return true;
}

struct SnapshotWriter {
  HalFile* file = nullptr;
  bool hasSchedule = false;
  bool hasConfig = false;
  bool hasContent = false;
};

bool writeSnapshotEntry(void* context, const project_stick::ReleaseFileEntry& entry) {
  auto* writer = static_cast<SnapshotWriter*>(context);
  if (!writer || !writer->file || !project_stick::isSafeReleasePath(entry.path) ||
      entry.path.size() > MAX_RELEASE_PATH || !validSha256(entry.sha256) || entry.size == 0 ||
      entry.size > MAX_RELEASE_FILE_SIZE) {
    return false;
  }
  writer->hasSchedule = writer->hasSchedule || entry.path == "schedule.json";
  writer->hasConfig = writer->hasConfig || entry.path == "config.json";
  writer->hasContent = writer->hasContent || entry.path.rfind("content/", 0) == 0;
  char sizeText[24];
  const int length = snprintf(sizeText, sizeof(sizeText), "%zu", entry.size);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(sizeText)) return false;
  const char tab = '\t';
  const char newline = '\n';
  return writer->file->write(entry.sha256.data(), entry.sha256.size()) == entry.sha256.size() &&
         writer->file->write(&tab, 1) == 1 &&
         writer->file->write(sizeText, static_cast<size_t>(length)) == static_cast<size_t>(length) &&
         writer->file->write(&tab, 1) == 1 &&
         writer->file->write(entry.path.data(), entry.path.size()) == entry.path.size() &&
         writer->file->write(&newline, 1) == 1;
}

class JsonObjectValidator {
 public:
  JsonObjectValidator()
      : callbacks{this, nullptr, nullptr, nullptr, nullptr, nullptr, onObjectStart, onContainerEnd,
                  onArrayStart, onContainerEnd},
        parser(callbacks) {}

  void feed(const char* data, size_t length) { parser.feed(data, length); }
  bool finish() const { return rootSeen && !invalid && !parser.hasError() && level == 0; }

 private:
  static void onObjectStart(void* context) {
    auto* self = static_cast<JsonObjectValidator*>(context);
    if (self->level == 0) {
      if (self->rootSeen) self->invalid = true;
      self->rootSeen = true;
    }
    ++self->level;
  }

  static void onArrayStart(void* context) {
    auto* self = static_cast<JsonObjectValidator*>(context);
    if (self->level == 0) self->invalid = true;
    ++self->level;
  }

  static void onContainerEnd(void* context) {
    auto* self = static_cast<JsonObjectValidator*>(context);
    if (self->level == 0) {
      self->invalid = true;
    } else {
      --self->level;
    }
  }

  JsonCallbacks callbacks;
  StreamingJsonParser parser;
  uint8_t level = 0;
  bool rootSeen = false;
  bool invalid = false;
};
}  // namespace

void ProjectStickService::begin() {
  baseUrl = PROJECT_STICK_BASE_URL;
  while (!baseUrl.empty() && baseUrl.back() == '/') baseUrl.pop_back();
  PROJECT_STICK_STORE.loadFromFile();
  if (ensureIdentity()) PROJECT_STICK_STORE.saveToFile();
  cleanupReleaseStorage();
}

bool ProjectStickService::ensureIdentity() {
  if (!PROJECT_STICK_STORE.deviceId.empty()) return false;
  PROJECT_STICK_STORE.deviceId = makeUuid();
  LOG_INF("STICK", "Created device identity %s", PROJECT_STICK_STORE.deviceId.c_str());
  return true;
}

ProjectStickService::SyncReport ProjectStickService::sync(bool registerFirst) {
  SyncReport report;
  inactive = false;
  if (registerFirst) {
    report.registerAttempted = true;
    int status = 0;
    if (!registerDevice(status)) {
      if (status == 403) {
        inactive = true;
        report.result = SyncResult::Inactive;
        return report;
      }
      report.result = refreshScheduledContent() ? SyncResult::OfflineCache : SyncResult::Failed;
      return report;
    }
    report.registerSucceeded = true;
  }

  flushEvents();
  report.manifestAttempted = true;
  report.result = syncManifest();
  report.manifestCompleted =
      report.result == SyncResult::Updated || report.result == SyncResult::Unchanged;
  if (report.result == SyncResult::Updated ||
      (report.result == SyncResult::Unchanged && currentDisplay.copyId == 0)) {
    if (!refreshScheduledContent()) {
      report.result =
          project_stick::contentSelectionFailureResult(PROJECT_STICK_STORE.activeVersion);
    }
  } else if (report.result == SyncResult::OfflineCache && currentDisplay.copyId == 0 &&
             !refreshScheduledContent()) {
    // A failed manifest request with no usable cache is a connection/storage
    // failure, not evidence that the server has no published release.
    report.result = SyncResult::Failed;
  }
  if (report.manifestCompleted && currentDisplay.copyId != 0) flushEvents();
  return report;
}

bool ProjectStickService::registerDevice(int& status) {
  LOG_INF("STICK", "Registering device (firmware=%s)", CROSSPOINT_VERSION);
  JsonDocument request;
  request["device_id"] = PROJECT_STICK_STORE.deviceId;
  request["firmware_version"] = CROSSPOINT_VERSION;
  std::string body;
  serializeJson(request, body);

  std::string response;
  if (!requestPost("/api/v1/device/register", body, response, status) || status != 200) {
    LOG_ERR("STICK", "Device registration failed (status=%d)", status);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    LOG_ERR("STICK", "Device registration returned invalid JSON");
    return false;
  }

  PROJECT_STICK_STORE.pollIntervalSeconds =
      std::clamp<uint32_t>(doc["poll_interval_seconds"] | 300, 30, 86400);
  PROJECT_STICK_STORE.alertPollIntervalSeconds =
      std::clamp<uint32_t>(doc["alert_poll_interval_seconds"] | 30, 10, 3600);
  PROJECT_STICK_STORE.tradingDay = doc["is_trading_day"] | false;
  parseServerTime(doc["server_time"] | "");
  if (!PROJECT_STICK_STORE.saveToFile()) {
    LOG_ERR("STICK", "Device registration state could not be saved to SD");
    return false;
  }
  LOG_INF("STICK", "Device registered (release=%u poll=%us alerts=%us)",
          (unsigned)(doc["current_release_version"] | 0),
          (unsigned)PROJECT_STICK_STORE.pollIntervalSeconds,
          (unsigned)PROJECT_STICK_STORE.alertPollIntervalSeconds);
  return true;
}

ProjectStickService::SyncResult ProjectStickService::syncManifest() {
  const std::string path = "/api/v1/device/manifest?device_id=" + PROJECT_STICK_STORE.deviceId +
                           "&current_version=" + std::to_string(PROJECT_STICK_STORE.activeVersion);
  if ((!Storage.mkdir(DATA_ROOT, true) && !Storage.exists(DATA_ROOT)) ||
      (!Storage.mkdir(SNAPSHOT_ROOT, true) && !Storage.exists(SNAPSHOT_ROOT))) {
    return SyncResult::Failed;
  }
  LOG_INF("STICK", "Fetching manifest (current=%u)", (unsigned)PROJECT_STICK_STORE.activeVersion);
  if (!fetchToFile(baseUrl + path, MANIFEST_TEMP, MAX_MANIFEST_BYTES)) {
    LOG_ERR("STICK", "Manifest request failed");
    return SyncResult::OfflineCache;
  }

  Storage.remove(SNAPSHOT_TEMP);
  HalFile manifest;
  HalFile snapshot;
  if (!Storage.openFileForRead("STICK", MANIFEST_TEMP, manifest) ||
      !Storage.openFileForWrite("STICK", SNAPSHOT_TEMP, snapshot)) {
    Storage.remove(MANIFEST_TEMP);
    return SyncResult::Failed;
  }
  SnapshotWriter writer{&snapshot};
  if (!hasParserHeap()) {
    manifest.close();
    snapshot.close();
    Storage.remove(MANIFEST_TEMP);
    Storage.remove(SNAPSHOT_TEMP);
    return SyncResult::Failed;
  }
  auto decoder =
      makeUniqueNoThrow<project_stick::ReleaseManifestDecoder>(writeSnapshotEntry, &writer);
  auto buffer = makeUniqueNoThrow<char[]>(512);
  if (!decoder || !buffer) {
    LOG_ERR("STICK", "OOM: manifest stream parser");
    manifest.close();
    snapshot.close();
    Storage.remove(MANIFEST_TEMP);
    Storage.remove(SNAPSHOT_TEMP);
    return SyncResult::Failed;
  }
  while (manifest.available()) {
    const int count = manifest.read(buffer.get(), 512);
    if (count <= 0) break;
    decoder->feed(buffer.get(), static_cast<size_t>(count));
  }
  snapshot.flush();
  manifest.close();
  snapshot.close();
  Storage.remove(MANIFEST_TEMP);
  if (!decoder->finish() ||
      (!decoder->unchanged() && (!writer.hasSchedule || !writer.hasConfig || !writer.hasContent))) {
    LOG_ERR("STICK", "Manifest is invalid or incomplete");
    Storage.remove(SNAPSHOT_TEMP);
    return SyncResult::Failed;
  }
  if (!decoder->serverTime().empty()) parseServerTime(decoder->serverTime().c_str());
  if (decoder->pollIntervalSeconds() != 0) {
    PROJECT_STICK_STORE.pollIntervalSeconds =
        std::clamp<uint32_t>(decoder->pollIntervalSeconds(), 30, 86400);
  }
  if (decoder->alertPollIntervalSeconds() != 0) {
    PROJECT_STICK_STORE.alertPollIntervalSeconds =
        std::clamp<uint32_t>(decoder->alertPollIntervalSeconds(), 10, 3600);
  }
  const uint32_t version = decoder->version();
  if (decoder->unchanged()) {
    LOG_INF("STICK", "Manifest unchanged (version=%u)", (unsigned)version);
    Storage.remove(SNAPSHOT_TEMP);
    PROJECT_STICK_STORE.saveToFile();
    return SyncResult::Unchanged;
  }
  if (version != PROJECT_STICK_STORE.activeVersion && PROJECT_STICK_STORE.previousVersion != 0) {
    const uint32_t obsolete = PROJECT_STICK_STORE.previousVersion;
    PROJECT_STICK_STORE.previousVersion = 0;
    if (!PROJECT_STICK_STORE.saveToFile()) {
      PROJECT_STICK_STORE.previousVersion = obsolete;
      Storage.remove(SNAPSHOT_TEMP);
      return SyncResult::Failed;
    }
    Storage.removeDir(releaseRoot(obsolete).c_str());
    cleanupReleaseStorage(true);
  }
  const std::string finalSnapshot = snapshotFile(version);
  Storage.remove(finalSnapshot.c_str());
  if (!Storage.rename(SNAPSHOT_TEMP, finalSnapshot.c_str())) {
    Storage.remove(SNAPSHOT_TEMP);
    return SyncResult::Failed;
  }

  if (!materializeRelease(version)) {
    LOG_ERR("STICK", "Release %u download/validation failed", (unsigned)version);
    return SyncResult::Failed;
  }
  if (!activateSnapshot(version)) {
    LOG_ERR("STICK", "Release %u activation failed", (unsigned)version);
    return SyncResult::Failed;
  }
  scheduleCache.clear();
  cleanupReleaseStorage();
  queueEvent("sync_completed", "", 0);
  flushEvents();
  LOG_INF("STICK", "Release %u synchronized and activated", (unsigned)version);
  return SyncResult::Updated;
}

bool ProjectStickService::materializeRelease(uint32_t version) {
  if (!Storage.mkdir(OBJECT_ROOT, true) && !Storage.exists(OBJECT_ROOT)) return false;
  HalFile snapshot;
  if (!Storage.openFileForRead("STICK", snapshotFile(version), snapshot)) return false;
  while (snapshot.available()) {
    project_stick::ReleaseFileEntry entry;
    if (!readSnapshotEntry(snapshot, entry) || entry.size > MAX_RELEASE_FILE_SIZE ||
        !ensureObject(version, entry) || !validateObject(entry)) {
      return false;
    }
  }
  snapshot.close();

  project_stick::ReleaseFileEntry scheduleEntry;
  if (!findSnapshotEntry(version, "schedule.json", scheduleEntry) || !hasParserHeap()) return false;
  auto schedule = makeUniqueNoThrow<project_stick::ScheduleStreamDecoder>();
  if (!schedule || !streamScheduleFile(objectFile(scheduleEntry.sha256), *schedule) || !schedule->finish()) {
    return false;
  }
  auto windows = schedule->takeWindows();
  schedule.reset();
  for (const auto& window : windows) {
    if (!window.enabled) continue;
    const std::string contentPath = "content/" + window.scenario + ".json";
    project_stick::ReleaseFileEntry contentEntry;
    if (!project_stick::isSafeReleasePath(contentPath) ||
        !findSnapshotEntry(version, contentPath, contentEntry) ||
        !Storage.exists(objectFile(contentEntry.sha256).c_str())) {
      LOG_ERR("STICK", "Release missing scheduled content: %s", contentPath.c_str());
      return false;
    }
    const std::vector<int64_t> noUsedIds;
    auto content = makeUniqueNoThrow<project_stick::ContentStreamDecoder>(
        project_stick::ContentPassMode::MEASURE, noUsedIds);
    if (!content || !streamContentFile(objectFile(contentEntry.sha256), *content) || !content->finish()) {
      LOG_ERR("STICK", "Release has no displayable scheduled content: %s", contentPath.c_str());
      return false;
    }
  }
  return true;
}

bool ProjectStickService::ensureObject(uint32_t version, const project_stick::ReleaseFileEntry& file) {
  const std::string destination = objectFile(file.sha256);
  std::string existingHash;
  if (fileWithinLimit(destination, file.size) && hashFile(destination, existingHash) &&
      equalsIgnoreCase(existingHash, file.sha256)) {
    return true;
  }
  Storage.remove(destination.c_str());

  const std::string legacy = releaseFile(PROJECT_STICK_STORE.activeVersion, file.path);
  if (PROJECT_STICK_STORE.activeVersion != 0 && Storage.exists(legacy.c_str()) &&
      fileWithinLimit(legacy, file.size) && hashFile(legacy, existingHash) &&
      equalsIgnoreCase(existingHash, file.sha256)) {
    const std::string temporary = destination + ".tmp";
    Storage.remove(temporary.c_str());
    if (copyFile(legacy, temporary) && Storage.rename(temporary.c_str(), destination.c_str())) return true;
    Storage.remove(temporary.c_str());
  }
  return downloadObject(version, file);
}

bool ProjectStickService::downloadObject(uint32_t version, const project_stick::ReleaseFileEntry& file) {
  const std::string destination = objectFile(file.sha256);
  const std::string temporary = destination + ".tmp";
  const std::string url = baseUrl + "/api/v1/device/releases/" + std::to_string(version) + "/" + file.path +
                          "?device_id=" + PROJECT_STICK_STORE.deviceId;
  LOG_INF("STICK", "Downloading %s (%u bytes)", file.path.c_str(), (unsigned)file.size);
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    Storage.remove(temporary.c_str());
    HalFile output;
    if (!Storage.openFileForWrite("STICK", temporary, output)) return false;
    size_t received = 0;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    const bool fetched = HttpDownloader::fetchUrl(url, [&output, &received, &context, &file](
                                                           const uint8_t* data, size_t length) {
      if (received > file.size || length > file.size - received) return false;
      if (output.write(data, length) != length) return false;
      mbedtls_sha256_update(&context, data, length);
      received += length;
      return true;
    });
    output.flush();
    output.close();
    uint8_t digest[32];
    mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    char hex[65];
    for (size_t i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = '\0';
    if (fetched && received == file.size && equalsIgnoreCase(hex, file.sha256)) {
      Storage.remove(destination.c_str());
      if (Storage.rename(temporary.c_str(), destination.c_str())) {
        LOG_INF("STICK", "Stored %s", file.path.c_str());
        return true;
      }
    }
    Storage.remove(temporary.c_str());
    if (attempt < 2) delay(250UL << attempt);
  }
  LOG_ERR("STICK", "Failed release object: %s", file.path.c_str());
  return false;
}

bool ProjectStickService::validateObject(const project_stick::ReleaseFileEntry& file) {
  const std::string path = objectFile(file.sha256);
  if (!hasParserHeap()) return false;
  if (file.path == "schedule.json") {
    if (file.size > MAX_SCHEDULE_BYTES) return false;
    auto decoder = makeUniqueNoThrow<project_stick::ScheduleStreamDecoder>();
    return decoder && streamScheduleFile(path, *decoder) && decoder->finish();
  }
  if (file.path.rfind("content/", 0) == 0) {
    if (file.size > MAX_CONTENT_BYTES) return false;
    const std::vector<int64_t> noUsedIds;
    auto decoder = makeUniqueNoThrow<project_stick::ContentStreamDecoder>(
        project_stick::ContentPassMode::MEASURE, noUsedIds);
    return decoder && streamContentFile(path, *decoder) && decoder->finishAllowEmpty();
  }

  HalFile input;
  if (!Storage.openFileForRead("STICK", path, input)) return false;
  auto validator = makeUniqueNoThrow<JsonObjectValidator>();
  auto buffer = makeUniqueNoThrow<char[]>(512);
  if (!validator || !buffer) return false;
  while (input.available()) {
    const int count = input.read(buffer.get(), 512);
    if (count <= 0) return false;
    validator->feed(buffer.get(), static_cast<size_t>(count));
  }
  return validator->finish();
}

bool ProjectStickService::activateSnapshot(uint32_t version) {
  const uint32_t oldActive = PROJECT_STICK_STORE.activeVersion;
  const uint32_t oldPrevious = PROJECT_STICK_STORE.previousVersion;
  PROJECT_STICK_STORE.previousVersion = oldActive == version ? oldPrevious : oldActive;
  PROJECT_STICK_STORE.activeVersion = version;
  if (PROJECT_STICK_STORE.saveToFile()) return true;
  PROJECT_STICK_STORE.activeVersion = oldActive;
  PROJECT_STICK_STORE.previousVersion = oldPrevious;
  return false;
}

bool ProjectStickService::loadSchedule(std::vector<project_stick::ScheduleWindow>& windows) {
  std::string path;
  if (!resolveReleaseFile(PROJECT_STICK_STORE.activeVersion, "schedule.json", path) ||
      !fileWithinLimit(path, MAX_SCHEDULE_BYTES)) {
    return false;
  }
  if (!hasParserHeap()) return false;
  auto decoder = makeUniqueNoThrow<project_stick::ScheduleStreamDecoder>();
  if (!decoder || !streamScheduleFile(path, *decoder) || !decoder->finish()) return false;
  windows = decoder->takeWindows();
  return !windows.empty();
}

bool ProjectStickService::selectContent(const std::string& scenario, uint32_t randomValue,
                                        project_stick::ContentCopy& selected) {
  const std::string relative = "content/" + scenario + ".json";
  if (!project_stick::isSafeReleasePath(relative)) return false;
  std::string path;
  if (!resolveReleaseFile(PROJECT_STICK_STORE.activeVersion, relative, path) ||
      !fileWithinLimit(path, MAX_CONTENT_BYTES)) {
    return false;
  }
  if (!hasParserHeap()) return false;
  auto measure = makeUniqueNoThrow<project_stick::ContentStreamDecoder>(
      project_stick::ContentPassMode::MEASURE, PROJECT_STICK_STORE.usedCopyIds);
  if (!measure || !streamContentFile(path, *measure) || !measure->finish()) return false;
  const bool useAll = measure->unusedWeight() == 0;
  const uint32_t total = useAll ? measure->totalWeight() : measure->unusedWeight();
  if (total == 0) return false;
  measure.reset();

  auto choose = makeUniqueNoThrow<project_stick::ContentStreamDecoder>(
      useAll ? project_stick::ContentPassMode::SELECT_ALL : project_stick::ContentPassMode::SELECT_UNUSED,
      PROJECT_STICK_STORE.usedCopyIds, randomValue % total);
  if (!choose || !streamContentFile(path, *choose) || !choose->finish() || !choose->selected()) return false;
  selected = choose->takeSelected();
  return true;
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

  if (current.valid && current.day != PROJECT_STICK_STORE.usedDay) {
    PROJECT_STICK_STORE.usedDay = current.day;
    PROJECT_STICK_STORE.usedCopyIds.clear();
  }
#ifdef SIMULATOR
  const uint32_t randomValue = static_cast<uint32_t>(std::rand());
#else
  const uint32_t randomValue = esp_random();
#endif
  // The e-paper retains the previous pixels; release its backing text before
  // streaming the next selection so only one full copy is resident at a time.
  std::string().swap(currentDisplay.text);
  std::string().swap(currentDisplay.tone);
  currentDisplay.copyId = 0;
  currentDisplay.alert = false;
  project_stick::ContentCopy selected;
  if (!selectContent(scenario, randomValue, selected)) return false;

  currentDisplay.scenario = scenario;
  currentDisplay.text = std::move(selected.text);
  currentDisplay.tone = std::move(selected.tone);
  currentDisplay.copyId = selected.id;
  currentDisplay.alert = forcedScenario && strcmp(forcedScenario, "volatility_alert") == 0;
  PROJECT_STICK_STORE.markCopyUsed(current.valid ? current.day : PROJECT_STICK_STORE.usedDay, selected.id);
  PROJECT_STICK_STORE.saveToFile();
  queueEvent("trigger_fired", scenario, selected.id);
  queueEvent("screen_view", scenario, selected.id);
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
  bool eligible = PROJECT_STICK_STORE.tradingDay && current.valid;
  const uint16_t minute = current.minuteOfDay();
  eligible =
      eligible && ((minute >= 570 && minute < 690) || (minute >= 780 && minute < 900));
#ifdef SIMULATOR
  eligible = eligible || std::getenv("CROSSPOINT_SIM_FORCE_ALERT_WINDOW") != nullptr;
#endif
  if (!eligible) return false;

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
        flushEvents();
        return true;
      }
    }
  }
  PROJECT_STICK_STORE.saveToFile();
  return false;
}

void ProjectStickService::sendFeedback(bool useful, bool canSend) {
  if (currentDisplay.copyId == 0) return;
  queueEvent(useful ? "feedback_useful" : "feedback_meh", currentDisplay.scenario, currentDisplay.copyId);
  if (canSend) flushEvents();
}

void ProjectStickService::queueManualRefresh() {
  queueEvent("manual_refresh", currentDisplay.scenario, currentDisplay.copyId);
}

bool ProjectStickService::sendManualRefresh(bool canSend) {
  queueManualRefresh();
  if (canSend) flushEvents();
  bool selected = false;
  if (currentDisplay.scenario.empty()) {
    selected = refreshScheduledContent("manual_refresh");
  } else {
    const std::string scenario = currentDisplay.scenario;
    selected = refreshScheduledContent(scenario.c_str());
  }
  if (canSend) flushEvents();
  return selected;
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
  status = 0;
#ifdef SIMULATOR
  static bool injectedRegisterFailure = false;
  if (!injectedRegisterFailure && path == "/api/v1/device/register" &&
      std::getenv("CROSSPOINT_SIM_FAIL_FIRST_REGISTER") != nullptr) {
    injectedRegisterFailure = true;
    LOG_ERR("STICK", "Simulator injected the first register failure");
    return false;
  }
#endif
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
#ifndef SIMULATOR
    LOG_INF("STICK", "POST %s attempt %u/3 (heap=%u max=%u)", path.c_str(),
            (unsigned)attempt + 1, (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMaxAllocHeap());
#else
    LOG_INF("STICK", "POST %s attempt %u/3 (simulator OpenSSL)", path.c_str(),
            (unsigned)attempt + 1);
#endif
    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
#ifndef SIMULATOR
    http.setUserAgent("Project.Stick-CrossPoint-" CROSSPOINT_VERSION);
    http.setFollowRedirects(3);
#endif
    if (!http.begin(baseUrl + path)) {
      LOG_ERR("STICK", "POST %s has an invalid URL", path.c_str());
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    status = http.sendRequest("POST", body);
    response = http.getString();
#ifdef SIMULATOR
    const bool complete = true;
#else
    const bool complete = http.responseComplete();
#endif
    http.end();
    if (status > 0 && complete && status < 500) {
      LOG_INF("STICK", "POST %s completed (status=%d bytes=%u)", path.c_str(),
              status, (unsigned)response.size());
      return true;
    }
    LOG_ERR("STICK", "POST %s failed (status=%d complete=%u bytes=%u)",
            path.c_str(), status, complete ? 1u : 0u, (unsigned)response.size());
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

bool ProjectStickService::fetchToFile(const std::string& url, const std::string& path, size_t maxBytes) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    Storage.remove(path.c_str());
    HalFile output;
    if (!Storage.openFileForWrite("STICK", path, output)) return false;
    size_t received = 0;
    const bool ok = HttpDownloader::fetchUrl(url, [&output, &received, maxBytes](const uint8_t* data,
                                                                                size_t length) {
      if (received > maxBytes || length > maxBytes - received) return false;
      if (output.write(data, length) != length) return false;
      received += length;
      return true;
    });
    output.flush();
    output.close();
    if (ok && received != 0) return true;
    Storage.remove(path.c_str());
    if (attempt < 2) delay(250UL << attempt);
  }
  return false;
}

bool ProjectStickService::streamScheduleFile(const std::string& path,
                                             project_stick::ScheduleStreamDecoder& decoder) {
  HalFile input;
  if (!Storage.openFileForRead("STICK", path, input)) return false;
  auto buffer = makeUniqueNoThrow<char[]>(512);
  if (!buffer) return false;
  while (input.available()) {
    const int count = input.read(buffer.get(), 512);
    if (count <= 0) return false;
    decoder.feed(buffer.get(), static_cast<size_t>(count));
  }
  return true;
}

bool ProjectStickService::streamContentFile(const std::string& path,
                                            project_stick::ContentStreamDecoder& decoder) {
  HalFile input;
  if (!Storage.openFileForRead("STICK", path, input)) return false;
  auto buffer = makeUniqueNoThrow<char[]>(512);
  if (!buffer) return false;
  while (input.available()) {
    const int count = input.read(buffer.get(), 512);
    if (count <= 0) return false;
    decoder.feed(buffer.get(), static_cast<size_t>(count));
  }
  return true;
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

bool ProjectStickService::findSnapshotEntry(uint32_t version, const std::string& path,
                                            project_stick::ReleaseFileEntry& result) {
  HalFile snapshot;
  if (version == 0 || !Storage.openFileForRead("STICK", snapshotFile(version), snapshot)) return false;
  while (snapshot.available()) {
    project_stick::ReleaseFileEntry entry;
    if (!readSnapshotEntry(snapshot, entry)) return false;
    if (entry.path == path) {
      result = std::move(entry);
      return true;
    }
  }
  return false;
}

bool ProjectStickService::snapshotReferencesHash(uint32_t version, const std::string& sha256) {
  HalFile snapshot;
  if (version == 0 || !Storage.openFileForRead("STICK", snapshotFile(version), snapshot)) return false;
  while (snapshot.available()) {
    project_stick::ReleaseFileEntry entry;
    if (!readSnapshotEntry(snapshot, entry)) return false;
    if (equalsIgnoreCase(entry.sha256, sha256)) return true;
  }
  return false;
}

bool ProjectStickService::resolveReleaseFile(uint32_t version, const std::string& path, std::string& result) {
  project_stick::ReleaseFileEntry entry;
  if (findSnapshotEntry(version, path, entry)) {
    const std::string object = objectFile(entry.sha256);
    if (Storage.exists(object.c_str()) && fileWithinLimit(object, entry.size)) {
      result = object;
      return true;
    }
    return false;
  }

  // One-time compatibility with releases written by firmware before the
  // content-addressed SD layout was introduced.
  const std::string legacy = releaseFile(version, path);
  if (Storage.exists(legacy.c_str())) {
    result = legacy;
    return true;
  }
  return false;
}

void ProjectStickService::cleanupReleaseStorage(bool keepIncoming) {
  Storage.remove(MANIFEST_TEMP);
  Storage.mkdir(SNAPSHOT_ROOT, true);
  Storage.mkdir(OBJECT_ROOT, true);

  HalFile snapshots = Storage.open(SNAPSHOT_ROOT);
  if (snapshots && snapshots.isDirectory()) {
    while (true) {
      HalFile child = snapshots.openNextFile();
      if (!child) break;
      char name[128];
      child.getName(name, sizeof(name));
      const bool directory = child.isDirectory();
      child.close();
      if (directory) continue;
      const char* base = strrchr(name, '/');
      base = base ? base + 1 : name;
      const std::string keepActive = std::to_string(PROJECT_STICK_STORE.activeVersion) + ".idx";
      const std::string keepPrevious = std::to_string(PROJECT_STICK_STORE.previousVersion) + ".idx";
      if (base != keepActive && base != keepPrevious && !(keepIncoming && strcmp(base, "incoming.tmp") == 0)) {
        const std::string full = std::string(SNAPSHOT_ROOT) + "/" + base;
        Storage.remove(full.c_str());
      }
    }
  }

  HalFile objects = Storage.open(OBJECT_ROOT);
  if (objects && objects.isDirectory()) {
    while (true) {
      HalFile child = objects.openNextFile();
      if (!child) break;
      char name[128];
      child.getName(name, sizeof(name));
      const bool directory = child.isDirectory();
      child.close();
      if (directory) continue;
      const char* base = strrchr(name, '/');
      base = base ? base + 1 : name;
      const std::string fileName(base);
      const bool object = fileName.size() == 69 && fileName.compare(64, 5, ".json") == 0;
      const std::string sha256 = object ? fileName.substr(0, 64) : "";
      if (!object ||
          (!snapshotReferencesHash(PROJECT_STICK_STORE.activeVersion, sha256) &&
           !snapshotReferencesHash(PROJECT_STICK_STORE.previousVersion, sha256))) {
        const std::string full = std::string(OBJECT_ROOT) + "/" + fileName;
        Storage.remove(full.c_str());
      }
    }
  }

  // Keep the legacy directory only while it is the sole rollback copy.
  if (Storage.exists(snapshotFile(PROJECT_STICK_STORE.activeVersion).c_str()) &&
      (PROJECT_STICK_STORE.previousVersion == 0 ||
       Storage.exists(snapshotFile(PROJECT_STICK_STORE.previousVersion).c_str()))) {
    Storage.removeDir((std::string(DATA_ROOT) + "/releases").c_str());
  }
}

std::string ProjectStickService::snapshotFile(uint32_t version) const {
  return std::string(SNAPSHOT_ROOT) + "/" + std::to_string(version) + ".idx";
}

std::string ProjectStickService::objectFile(const std::string& sha256) const {
  return std::string(OBJECT_ROOT) + "/" + sha256 + ".json";
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
  static std::mt19937 random(std::random_device{}());
  for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
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
