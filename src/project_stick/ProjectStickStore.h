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
  std::string clientTs;
  std::string studioTask, studioCard;
};

class ProjectStickStore : public PersistableStore<ProjectStickStore> {
 private:
  ProjectStickStore() = default;
  friend class PersistableStore<ProjectStickStore>;

 public:
  static constexpr size_t MAX_PENDING_EVENTS = 32;
  static constexpr size_t MAX_SEEN_ALERTS = 32;

  std::string ownerId;
  std::string deviceId;
  std::string deviceToken;
  std::string pairingCode;
  bool bound = false;
  uint32_t pollIntervalSeconds = 300;
  uint32_t alertPollIntervalSeconds = 30;
  // Studio target poll cadence (server `studio_poll_seconds`).
  uint32_t studioPollSeconds = 60;
  bool tradingDay = false;
  // End of the latest market alert; the Studio program shows its alert scene until then.
  project_stick::ShanghaiTime alertUntil;
  std::vector<int64_t> seenAlertIds;
  std::vector<ProjectStickEvent> pendingEvents;

  static const char* getFilePath() { return "/.crosspoint/project_stick.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasSeenAlert(int64_t id) const;
  void markAlertSeen(int64_t id);
  void enqueue(ProjectStickEvent event);
};

#define PROJECT_STICK_STORE ProjectStickStore::getInstance()
