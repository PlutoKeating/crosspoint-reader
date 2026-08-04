#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>
#include <ProjectStickCore.h>

#include <cstdint>
#include <string>
#include <vector>

struct ProjectStickEvent {
  std::string id;
  std::string type;
  std::string scenario;
  int64_t copyId = 0;
  std::string clientTs;
};

class ProjectStickStore : public PersistableStore<ProjectStickStore> {
 private:
  ProjectStickStore() = default;
  friend class PersistableStore<ProjectStickStore>;

 public:
  static constexpr size_t MAX_USED_IDS = 64;
  static constexpr size_t MAX_PENDING_EVENTS = 32;

  std::string deviceId;
  uint32_t activeVersion = 0;
  uint32_t previousVersion = 0;
  uint32_t pollIntervalSeconds = 300;
  uint32_t alertPollIntervalSeconds = 30;
  uint32_t contentRefreshIntervalSeconds = 600;
  bool tradingDay = false;
  int64_t usedDay = 0;
  project_stick::ShanghaiTime rotationAnchor;
  uint32_t displayVersion = 0;
  std::string displayScenario;
  std::string displayText;
  std::string displayTone;
  int64_t displayCopyId = 0;
  bool displayAlert = false;
  project_stick::ShanghaiTime displayAlertUntil;
  std::vector<int64_t> usedCopyIds;
  std::vector<int64_t> seenAlertIds;
  std::vector<ProjectStickEvent> pendingEvents;

  static const char* getFilePath() { return "/.crosspoint/project_stick.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void markCopyUsed(int64_t day, int64_t id);
  bool hasSeenAlert(int64_t id) const;
  void markAlertSeen(int64_t id);
  void enqueue(ProjectStickEvent event);
};

#define PROJECT_STICK_STORE ProjectStickStore::getInstance()
