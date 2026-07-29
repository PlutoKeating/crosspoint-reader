#pragma once

#include <ProjectStickSyncState.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <mutex>

#include "project_stick/ProjectStickService.h"

class ProjectStickBackgroundSync {
 public:
  enum class WorkKind : uint8_t { None, Sync, AlertPoll };

  struct Result {
    uint32_t sequence = 0;
    WorkKind kind = WorkKind::None;
    project_stick::SyncReport syncReport;
    bool alertReceived = false;
    ProjectStickService::Display alertDisplay;
  };

  static ProjectStickBackgroundSync& getInstance();

  void begin();
  bool requestSync(bool registerFirst);
  bool requestAlertPoll();
  bool busy() const;
  uint32_t latestSequence() const;
  bool takeResult(uint32_t& lastSequence, Result& result) const;

 private:
  ProjectStickBackgroundSync() = default;
  ProjectStickBackgroundSync(const ProjectStickBackgroundSync&) = delete;
  ProjectStickBackgroundSync& operator=(const ProjectStickBackgroundSync&) = delete;

  static void taskTrampoline(void* context);
  [[noreturn]] void taskLoop();
  bool queue(WorkKind kind, bool registerFirst);

  mutable portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
  project_stick::BackgroundWorkGate gate;
  WorkKind pendingKind = WorkKind::None;
  bool pendingRegisterFirst = false;
  bool started = false;
  TaskHandle_t taskHandle = nullptr;

  mutable std::mutex resultMutex;
  Result latestResult;
  ProjectStickService service;
};

#define PROJECT_STICK_BACKGROUND_SYNC ProjectStickBackgroundSync::getInstance()
