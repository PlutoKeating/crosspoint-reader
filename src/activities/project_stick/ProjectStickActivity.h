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
  bool longPressFired = false;
  uint32_t lastManifestPollMs = 0;
  uint32_t lastAlertPollMs = 0;
  uint32_t lastScheduleCheckMs = 0;
  uint32_t lastRegisterMs = 0;
  char statusLine[96] = {0};

  void launchWifiSelection();
  void runInitialSync();
  void updateState(ProjectStickService::SyncResult result);
  void setStatus(const char* text);
};
