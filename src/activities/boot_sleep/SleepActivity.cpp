#include "SleepActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include "images/MoonIcon.h"
#include "project_stick/StudioFrame.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  // A streamed card (2.7.4) is on the panel but in no framebuffer. Rebuild it
  // from the SD card so the moon lands on it and the quick-resume frame saved
  // next matches the panel. Without one (no memory, no card) leave the panel
  // as it is: no moon, and the next boot shows the splash.
  if (!renderer.hasFrameBuffer() &&
      !(renderer.ensureFrameBuffer() && StudioFrame::instance().render(renderer))) {
    renderer.releaseFrameBuffer();
    return;
  }
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  if (gpio.deviceIsX3()) {
    // The controller still holds the displayed page, so its differential base
    // waveform can add the moon without a full-screen flash.
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}
