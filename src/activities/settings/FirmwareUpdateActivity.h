#pragma once

#include <string>

#include "activities/Activity.h"
#include "project_stick/ProjectStickService.h"

// Settings > System > Firmware update. Shows the installed version, checks the
// StockStick catalogue on demand and installs a newer build through the same
// command pipeline the mini program uses; the background sync task does all
// network and flash work while this screen renders its progress.
class FirmwareUpdateActivity final : public Activity {
 public:
  explicit FirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FirmwareUpdate", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Checking || state == State::Installing; }

 private:
  enum class State : uint8_t { Idle, Checking, Result, Confirming, Installing };

  void startCheck();
  void startInstall();
  void pollBackground();

  State state = State::Idle;
  ProjectStickService::FirmwareOffer offer;
  bool checkFailedOffline = false;
  bool rolledBackNotice = false;
  std::string rolledBackVersion;
  uint32_t backgroundSequence = 0;
  uint32_t renderedProgressGeneration = 0;
  bool awaitingWork = false;
  // The screen opens on the Confirm release that selected it; only act on
  // buttons pressed while it is showing.
  bool confirmPressSeen = false;
  bool backPressSeen = false;
};
