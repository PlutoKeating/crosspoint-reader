#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/StickOverlays.h"
#include "project_stick/ProjectStickHost.h"
#include "project_stick/ProjectStickService.h"
#include "project_stick/StudioBluetooth.h"

// The StockStick page: the current card (or the setup QR / status screen),
// its key hints, feedback bubbles and status notices. It only renders and
// handles its keys; everything a phone or the cloud drives lives in
// ProjectStickHost and keeps running on every other page.
class ProjectStickActivity final : public Activity {
 public:
  explicit ProjectStickActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ProjectStick", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool needsFullPower() override;
  // The device-wide keyguard applies here too; this page draws its overlay so a
  // card change underneath still shows while locked.
  bool composesKeyguardOverlay() const override { return true; }
  bool skipLoopDelay() override;
  // Cards stream to the panel without the framebuffer (2.7.4); this page
  // takes it back only for the status screen and the firmware screen.
  bool ownsFrameBuffer() const override { return true; }
  bool handleForcedRefresh() override;
  StickTakeover stickTakeover() const override { return StickTakeover::StickPage; }

 private:
  ProjectStickService& service = PROJECT_STICK_HOST.service();
  uint32_t hostGeneration = 0;
  uint32_t hostEventSequence = 0;
  uint32_t studioGeneration = 0;
  uint32_t firmwareUpdateGeneration = 0;
  // Front button hints appear on a key action and hide after
  // BUTTON_HINT_TIMEOUT_MS without one; never while locked.
  static constexpr uint32_t BUTTON_HINT_TIMEOUT_MS = 5000;
  bool buttonHintsVisible = false;
  bool keyguardWasLocked = false;
  uint32_t lastKeyActionMs = 0;
  void updateButtonHints(uint32_t nowMs);
  // Feedback window after a side key: slides in from that key's edge.
  stick_overlay::Bubble feedbackBubble = stick_overlay::Bubble::None;
  uint32_t feedbackBubbleStartedMs = 0;
  uint8_t feedbackBubbleFrame = 0;
  void showFeedbackBubble(stick_overlay::Bubble bubble);
  void updateFeedbackBubble();
  // Everything the overlays draw, read once per repaint: a streamed card
  // draws them for every strip of every plane, and they must not change in
  // between (the controller's baseline would then differ from the panel).
  struct Layers {
    bool locked = false;
    project_stick::Keyguard::State keyguard{};
    bool keyguardPrompt = false;
    uint8_t keyguardCue = 0;
    bool hints = false;
    stick_overlay::Bubble bubble = stick_overlay::Bubble::None;
    uint8_t bubbleFrame = 0;
    stick_overlay::Notice notice = stick_overlay::Notice::None;
    int noticePercent = 0;
    uint8_t noticeFrame = 0;
  } layers;
  void captureLayers();
  void drawOverlays(bool studio) const;
  static void drawCardLayers(const GfxRenderer& renderer, void* self);
  // A short power press while a streamed card is shown: repaint it HALF.
  bool forcedRefreshPending = false;
  // Status notice over the card or status screen: live work (a phone link, a
  // transfer with progress, a Wi-Fi join, the cloud heartbeat) and its result.
  static constexpr uint32_t NOTICE_RESULT_MS = 3000;
  static constexpr uint32_t RECEIVING_FRAME_MS = 3000;
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
  void renderFirmwareUpdate();
  void renderStatusScreen();
  void launchWifiSelection();
};
