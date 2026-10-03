#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/StickOverlays.h"
#include "network/WifiAutoConnect.h"
#include "project_stick/ProjectStickBackgroundSync.h"
#include "project_stick/ProjectStickService.h"
#include "project_stick/StudioBluetooth.h"

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
  std::string setupPayload;
  std::string bleWifiSsid;
  bool bleWifiActive = false;
  bool bleScanActive = false;
  bool bleScanPending = false;
  bool syncAfterBle = false;
  // An OTA the phone requested over BLE, held until the worker accepts it.
  bool otaPending = false;
  ProjectStickService::FirmwareTarget otaTarget;
  project_stick::Deadline otaQueueDeadline;
  uint32_t bleWifiStartedMs = 0;
  uint32_t bleScanStartedMs = 0;
  // Status notice over the card or status screen: live work (a phone link, a
  // transfer with progress, a Wi-Fi join, the cloud heartbeat) and its result.
  static constexpr uint32_t NOTICE_RESULT_MS = 3000;
  static constexpr uint32_t NOTICE_FAILURE_MS = 6000;
  stick_overlay::Notice notice = stick_overlay::Notice::None;
  int noticePercent = 0;
  uint8_t noticeFrame = 0;
  uint32_t noticeFrameMs = 0;
  // A result shown for a fixed time once the live work ends.
  stick_overlay::Notice transientNotice = stick_overlay::Notice::None;
  uint32_t transientNoticeUntilMs = 0;
  studio_ble::Transfer lastTransfer = studio_ble::Transfer::None;
  studio_ble::Radio lastRadio = studio_ble::Radio::Idle;
  uint32_t seenFailedTransfers = 0;
  bool syncNoticeShown = false;
  void showTransientNotice(stick_overlay::Notice result, uint32_t durationMs);
  void updateNotice(uint32_t nowMs);
  void tickBleSetup();
  void startBleWifi(const std::string& ssid, const std::string& password);
  void pollBleWifi();
  void startBleScan();
  void pollBleScan();
  void renderFirmwareUpdate();
  void renderStatusScreen();
  uint32_t lastSyncAttemptMs = 0;
  uint32_t lastAlertPollMs = 0;
  uint32_t lastRegisterMs = 0;
  // BLE STATE characteristic refresh: often while a phone is connected, rarely otherwise.
  static constexpr uint32_t STATE_REFRESH_LINKED_MS = 2000;
  static constexpr uint32_t STATE_REFRESH_IDLE_MS = 30000;
  uint32_t lastStateRefreshMs = 0;
  void refreshBleState(uint32_t nowMs);
#ifdef SIMULATOR
  bool simulatorAlertPollPending = false;
#endif

  void applyBackgroundResult();
  // The cloud heartbeat; skipped while a phone sync is fresh unless `manual`
  // (the user closed the Wi-Fi screen, which is an explicit "sync now").
  bool requestCloudSync(bool manual = false);
  void updateState(const project_stick::SyncReport& report);
  void launchWifiSelection();
};
