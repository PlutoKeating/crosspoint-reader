#pragma once

#include "activities/Activity.h"
#include "project_stick/ProjectStickService.h"

class ProjectStickActivity final : public Activity {
 public:
  explicit ProjectStickActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ProjectStick", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { Connecting, Online, Offline, Empty, Error, Inactive };

  ProjectStickService service;
  State state = State::Connecting;
  bool workPending = false;
  bool manualCloudSyncPending = false;
  bool cloudSyncInProgress = false;
  uint32_t lastManifestPollMs = 0;
  uint32_t lastManifestAttemptMs = 0;
  uint32_t lastAlertPollMs = 0;
  uint32_t lastScheduleCheckMs = 0;
  uint32_t lastRegisterMs = 0;
  project_stick::SyncReport lastSyncReport;
  char statusLine[96] = {0};
#ifdef SIMULATOR
  bool simulatorRecoveryPending = false;
  bool simulatorAlertPollPending = false;
#endif

  void runInitialSync();
  void runManualRefresh();
  void runPendingManualCloudSync();
  void updateState(const project_stick::SyncReport& report);
  void recordSyncTiming(const project_stick::SyncReport& report);
  void launchWifiSelection();
  void setStatus(const char* text);
};
