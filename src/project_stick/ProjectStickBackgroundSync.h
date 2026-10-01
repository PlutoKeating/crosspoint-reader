#pragma once

#include <ProjectStickSyncState.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <mutex>
#include <string>

#include "project_stick/ProjectStickService.h"

class ProjectStickBackgroundSync {
 public:
  enum class WorkKind : uint8_t { None, Sync, AlertPoll, FirmwareCheck, FirmwareInstall };

  struct Result {
    uint32_t sequence = 0;
    WorkKind kind = WorkKind::None;
    project_stick::SyncReport syncReport;
    bool alertReceived = false;
    ProjectStickService::Display alertDisplay;
    ProjectStickService::FirmwareOffer firmware;
  };

  static ProjectStickBackgroundSync& getInstance();

  // Starts the worker; called once at boot from setup(). Later calls are no-ops.
  void begin();
  bool requestSync();
  bool requestAlertPoll();
  bool requestFirmwareCheck();
  // Downloads, verifies and flashes `target` on the worker (restarts on success).
  bool requestFirmwareInstall(const ProjectStickService::FirmwareTarget& target);
  bool busy() const;
  // The job the worker is running right now (None when idle or only queued).
  WorkKind runningKind() const;
  uint32_t latestSequence() const;
  bool takeResult(uint32_t& lastSequence, Result& result) const;

 private:
  ProjectStickBackgroundSync() = default;
  ProjectStickBackgroundSync(const ProjectStickBackgroundSync&) = delete;
  ProjectStickBackgroundSync& operator=(const ProjectStickBackgroundSync&) = delete;

  // ESP-IDF FreeRTOS counts stack depth in bytes (StackType_t is uint8_t).
  // TLS handshakes run on this stack.
  static constexpr uint32_t TASK_STACK_BYTES = 8192;
  static void taskTrampoline(void* context);
  [[noreturn]] void taskLoop();
  bool queue(WorkKind kind, const ProjectStickService::FirmwareTarget& target = {});

  mutable portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
  project_stick::BackgroundWorkGate gate;
  WorkKind pendingKind = WorkKind::None;
  WorkKind activeKind = WorkKind::None;
  ProjectStickService::FirmwareTarget pendingTarget;
  bool started = false;
  TaskHandle_t taskHandle = nullptr;

  mutable std::mutex resultMutex;
  Result latestResult;
  ProjectStickService service;
};

#define PROJECT_STICK_BACKGROUND_SYNC ProjectStickBackgroundSync::getInstance()
