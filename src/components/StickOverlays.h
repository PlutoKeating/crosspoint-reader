#pragma once

#include <cstdint>

#include "ProjectStickKeyguard.h"

class GfxRenderer;

// Transient overlays drawn on top of whatever the X3 shows: key hints, the
// feedback windows and the Nokia-style keyguard prompt. They share one visual
// language (black rounded chips with a white halo, white cards for feedback)
// and never change the underlying content; the next full render removes them.
namespace stick_overlay {

// Physical side keys of the X3 in portrait: left edge (BTN_UP, "没啥用") and
// right edge (BTN_DOWN, "有用"), spanning y 155..235.
constexpr int SIDE_KEY_Y = 155;
constexpr int SIDE_KEY_HEIGHT = 80;

enum class Bubble : uint8_t { None, Meh, Useful };
constexpr uint32_t BUBBLE_FRAME_MS = 120;
constexpr uint32_t BUBBLE_VISIBLE_MS = 2200;
constexpr uint8_t BUBBLE_FINAL_FRAME = 3;

// Edge tabs at the side keys: thumb-down "没啥用" on the left, "有用"
// thumb-up on the right. Hidden while a feedback window slides over them.
void drawSideKeyHints(const GfxRenderer& renderer, bool visible = true);

// Chips aligned with the four physical front keys; empty labels leave the
// slot blank.
void drawFrontKeyHints(const GfxRenderer& renderer, const char* back, const char* confirm, const char* left,
                       const char* right);

// Feedback window sliding from its key's edge toward the centre; frame runs
// 0..BUBBLE_FINAL_FRAME.
void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame);

// Lock icon, plus (after a key press while locked) the unlock guide: step
// tabs 1 and 2 at the side keys and a prompt chip. Once the left step is done
// the left tab shows a check and chevrons walk toward the right key over
// Keyguard::CUE_FINAL_FRAME frames.
void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame);

}  // namespace stick_overlay
