#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool handlesKeyguard() { return false; }
  // True when render() draws the keyguard overlay itself while locked (so the
  // activity keeps updating its content underneath); otherwise the manager only
  // overlays the lock on the last framebuffer.
  virtual bool composesKeyguardOverlay() const { return false; }
  // Pages that may show a frame without the framebuffer (a streamed card)
  // manage it themselves; for every other page the manager makes sure it
  // exists before onEnter() and render().
  virtual bool ownsFrameBuffer() const { return false; }
  // True while the page needs the CPU at full speed (live work: a firmware
  // update, a transfer, live radio diagnostics); otherwise the main loop drops
  // to idle power saving after HalPowerManager::IDLE_POWER_SAVING_MS without
  // input. There is no timed power-off (2.7.3): only the power key sleeps.
  virtual bool needsFullPower() { return false; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  // See StickTakeover: whether a phone's delivery may replace this page.
  virtual StickTakeover stickTakeover() const { return StickTakeover::Allowed; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);

 protected:
  enum class ListTouchResult : uint8_t {
    None,      // touch did not hit the list
    Consumed,  // touchdown moved the highlight (repaint already requested)
    Activated  // tap landed on a row: selectedIndex is updated, caller activates it
  };

  // Shared touch handling for selectable list screens: touchdown highlights the
  // touched row, a tap selects and reports Activated. The caller supplies the
  // list band and runs its own activate action on Activated.
  ListTouchResult handleListTouch(int& selectedIndex, int itemCount, int listTop, int listHeight, bool hasSubtitle);
};
