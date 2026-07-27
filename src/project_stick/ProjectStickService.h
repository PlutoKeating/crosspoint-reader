#pragma once

#include <ProjectStickCore.h>
#include <ProjectStickStream.h>

#include <cstdint>
#include <string>
#include <vector>

class ProjectStickService {
 public:
  enum class SyncResult { Updated, Unchanged, OfflineCache, NoContent, Failed, Inactive };

  struct Display {
    std::string scenario;
    std::string text;
    std::string tone;
    int64_t copyId = 0;
    bool alert = false;
  };

  void begin();
  SyncResult sync(bool registerFirst = true);
  bool pollAlerts();
  bool refreshIfScheduleChanged();
  bool refreshScheduledContent(const char* forcedScenario = nullptr);
  void sendFeedback(bool useful);
  void sendManualRefresh();

  const Display& display() const { return currentDisplay; }
  uint32_t activeVersion() const;
  uint32_t pendingEventCount() const;
  uint32_t pollIntervalSeconds() const;
  uint32_t alertPollIntervalSeconds() const;
  bool isTradingDay() const;
  bool hasClock() const { return serverTime.valid; }
  project_stick::ShanghaiTime now() const;

 private:
  std::string baseUrl;
  Display currentDisplay;
  project_stick::ShanghaiTime serverTime;
  uint32_t serverTimeCapturedMs = 0;
  bool inactive = false;
  std::vector<project_stick::ScheduleWindow> scheduleCache;

  bool ensureIdentity();
  bool registerDevice(int& status);
  SyncResult syncManifest();
  bool materializeRelease(uint32_t version);
  bool ensureObject(uint32_t version, const project_stick::ReleaseFileEntry& file);
  bool downloadObject(uint32_t version, const project_stick::ReleaseFileEntry& file);
  bool validateObject(const project_stick::ReleaseFileEntry& file);
  bool activateSnapshot(uint32_t version);
  void cleanupReleaseStorage(bool keepIncoming = false);
  bool loadSchedule(std::vector<project_stick::ScheduleWindow>& windows);
  bool selectContent(const std::string& scenario, uint32_t randomValue,
                     project_stick::ContentCopy& selected);
  bool streamScheduleFile(const std::string& path, project_stick::ScheduleStreamDecoder& decoder);
  bool streamContentFile(const std::string& path, project_stick::ContentStreamDecoder& decoder);
  bool fetchToFile(const std::string& url, const std::string& path, size_t maxBytes);
  bool requestPost(const std::string& path, const std::string& body, std::string& response, int& status);
  bool fetchJson(const std::string& url, std::string& response, size_t maxBytes);
  bool fileWithinLimit(const std::string& path, size_t maxBytes);
  bool parseServerTime(const char* value);
  bool hashFile(const std::string& path, std::string& result);
  bool copyFile(const std::string& source, const std::string& destination);
  bool findSnapshotEntry(uint32_t version, const std::string& path,
                         project_stick::ReleaseFileEntry& result);
  bool snapshotReferencesHash(uint32_t version, const std::string& sha256);
  bool resolveReleaseFile(uint32_t version, const std::string& path, std::string& result);
  std::string snapshotFile(uint32_t version) const;
  std::string objectFile(const std::string& sha256) const;
  std::string releaseRoot(uint32_t version) const;
  std::string releaseFile(uint32_t version, const std::string& path) const;
  void queueEvent(const char* type, const std::string& scenario, int64_t copyId);
  bool flushEvents();
  static std::string makeUuid();
};
