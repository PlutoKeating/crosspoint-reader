#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/StickOverlays.h"
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
  // The device-wide keyguard applies here too; this page draws its overlay so a
  // card change underneath still shows while locked.
  bool composesKeyguardOverlay() const override { return true; }
  bool skipLoopDelay() override;

 private:
  ProjectStickService service;
  // Deactivated in the console (register answered 403).
  bool inactive = false;
  uint32_t backgroundResultSequence = 0;
  uint32_t studioGeneration = 0;
  uint32_t firmwareUpdateGeneration = 0;
  // Front button hints appear on a key action and hide after
  // BUTTON_HINT_TIMEOUT_MS without one; never while locked.
  static constexpr uint32_t BUTTON_HINT_TIMEOUT_MS = 5000;
  bool buttonHintsVisible = false;
  uint32_t lastKeyActionMs = 0;
  void updateButtonHints(uint32_t nowMs);
  // Feedback window after a side key: slides in from that key's edge.
  stick_overlay::Bubble feedbackBubble = stick_overlay::Bubble::None;
  uint32_t feedbackBubbleStartedMs = 0;
  uint8_t feedbackBubbleFrame = 0;
  void showFeedbackBubble(stick_overlay::Bubble bubble);
  void updateFeedbackBubble();
  void drawOverlays(bool studio);
  WifiAutoConnect wifiAutoConnect;
  // BLE setup (protocol 3): bind, Wi-Fi join and scan requested by the phone.
  static constexpr uint32_t BLE_WIFI_TIMEOUT_MS = 20000;
  static constexpr uint32_t BLE_SCAN_TIMEOUT_MS = 15000;
  static constexpr uint32_t BLE_WIFI_MESSAGE_MS = 6000;
  std::string setupPayload;
  std::string bleWifiSsid;
  bool bleWifiActive = false;
  bool bleScanActive = false;
  bool bleScanPending = false;
  bool syncAfterBle = false;
  // An OTA the phone requested over BLE, held until the worker accepts it.
  bool otaPending = false;
  ProjectStickService::FirmwareTarget otaTarget;
  uint32_t bleWifiStartedMs = 0;
  uint32_t bleScanStartedMs = 0;
  // Transient status-screen line for a BLE Wi-Fi push (nullptr when none).
  const char* bleWifiMessage = nullptr;
  uint32_t bleWifiMessageUntilMs = 0;
  void tickBleSetup();
  void startBleWifi(const std::string& ssid, const std::string& password);
  void pollBleWifi();
  void startBleScan();
  void pollBleScan();
  void showBleWifiMessage(const char* message, uint32_t durationMs);
  void renderFirmwareUpdate();
  void renderStatusScreen();
  uint32_t lastSyncAttemptMs = 0;
  uint32_t lastAlertPollMs = 0;
  uint32_t lastRegisterMs = 0;
#ifdef SIMULATOR
  bool simulatorAlertPollPending = false;
#endif

  void applyBackgroundResult();
  bool requestCloudSync();
  void updateState(const project_stick::SyncReport& report);
  void launchWifiSelection();
};
