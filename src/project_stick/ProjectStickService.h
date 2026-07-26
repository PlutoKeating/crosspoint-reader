#pragma once

#include <ProjectStickCore.h>

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
  struct ManifestFile {
    std::string path;
    std::string sha256;
    size_t size = 0;
  };

  std::string baseUrl;
  Display currentDisplay;
  project_stick::ShanghaiTime serverTime;
  uint32_t serverTimeCapturedMs = 0;
  bool inactive = false;
  std::vector<project_stick::ScheduleWindow> scheduleCache;
  std::vector<project_stick::ContentCopy> contentCache;

  bool ensureIdentity();
  bool registerDevice(int& status);
  SyncResult syncManifest();
  bool materializeRelease(uint32_t version, const std::vector<ManifestFile>& files);
  bool loadSchedule(std::vector<project_stick::ScheduleWindow>& windows);
  bool loadContent(const std::string& scenario, std::vector<project_stick::ContentCopy>& copies);
  bool requestPost(const std::string& path, const std::string& body, std::string& response, int& status);
  bool fetchJson(const std::string& url, std::string& response, size_t maxBytes);
  bool fileWithinLimit(const std::string& path, size_t maxBytes);
  bool parseServerTime(const char* value);
  bool hashFile(const std::string& path, std::string& result);
  bool copyFile(const std::string& source, const std::string& destination);
  bool ensureParentDirectory(const std::string& path);
  std::string releaseRoot(uint32_t version) const;
  std::string releaseFile(uint32_t version, const std::string& path) const;
  void queueEvent(const char* type, const std::string& scenario, int64_t copyId);
  bool flushEvents();
  static std::string makeUuid();
};
