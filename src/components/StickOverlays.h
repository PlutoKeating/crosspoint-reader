#pragma once

#include <cstdint>

#include "ProjectStickKeyguard.h"

class GfxRenderer;

// Transient overlays drawn on top of whatever the X3 shows: side-key hints,
// the feedback bubbles and the Nokia-style keyguard prompt. They never change
// the underlying content; the next full render removes them.
namespace stick_overlay {

// Physical side keys of the X3 in portrait: left edge (BTN_UP, "没啥用") and
// right edge (BTN_DOWN, "有用").
constexpr int SIDE_KEY_MARGIN = 4;
constexpr int SIDE_KEY_WIDTH = 30;
constexpr int SIDE_KEY_HEIGHT = 80;
constexpr int SIDE_KEY_Y = 155;

enum class Bubble : uint8_t { None, Meh, Useful };
constexpr uint32_t BUBBLE_FRAME_MS = 120;
constexpr uint32_t BUBBLE_VISIBLE_MS = 2200;
constexpr uint8_t BUBBLE_FINAL_FRAME = 3;

// Outlined side-key boxes with thumbs icons, plus their labels unless a
// feedback bubble is about to slide over them.
void drawSideKeyHints(const GfxRenderer& renderer, bool labels = true);

// Feedback window sliding from its key's edge toward the centre; frame runs
// 0..BUBBLE_FINAL_FRAME.
void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame);

// Lock icon, plus (after a key press while locked) the unlock prompt: left and
// right keys marked 1 and 2; once the left step is done the prompt inverts and
// a cue slides toward the right key over Keyguard::CUE_FINAL_FRAME frames.
void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame);

}  // namespace stick_overlay
