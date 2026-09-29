#pragma once

#include "activities/Activity.h"
#include "network/WifiAutoConnect.h"
#include "project_stick/ProjectStickBackgroundSync.h"
#include "project_stick/ProjectStickService.h"

class ProjectStickActivity final : public Activity {
 public:
  explicit ProjectStickActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ProjectStick", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool allowIdlePowerSaving() override;
  bool handlesKeyguard() override;
  bool skipLoopDelay() override;

 private:
  enum class State { Connecting, Online, Offline, Error, Inactive };

  ProjectStickService service;
  State state = State::Connecting;
  uint32_t backgroundResultSequence = 0;
  uint32_t lastStudioPollMs = 0;
  uint32_t studioGeneration = 0;
  uint32_t firmwareUpdateGeneration = 0;
  // Front button hints appear on a key action and hide after
  // BUTTON_HINT_TIMEOUT_MS without one; never while locked.
  static constexpr uint32_t BUTTON_HINT_TIMEOUT_MS = 5000;
  bool buttonHintsVisible = false;
  uint32_t lastKeyActionMs = 0;
  void updateButtonHints(uint32_t nowMs);
  WifiAutoConnect wifiAutoConnect;
  void renderFirmwareUpdate();
  void renderStatusScreen();
  uint32_t lastSyncAttemptMs = 0;
  uint32_t lastAlertPollMs = 0;
  uint32_t lastRegisterMs = 0;
  char statusLine[96] = {0};
#ifdef SIMULATOR
  bool simulatorAlertPollPending = false;
#endif

  void applyBackgroundResult();
  bool requestCloudSync(bool registerFirst);
  void syncNow();
  void updateState(const project_stick::SyncReport& report);
  void launchWifiSelection();
  void setStatus(const char* text);
};
