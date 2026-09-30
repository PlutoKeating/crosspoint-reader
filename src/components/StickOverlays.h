#pragma once

#include <cstdint>

#include "ProjectStickKeyguard.h"

class GfxRenderer;

// The one implementation of every key hint on the device, plus the transient
// overlays of the card page (feedback windows, keyguard prompt). All share one
// light outline language: white fills, 2-px black outlines, regular-weight
// labels, and small solid triangles pointing at the physical key. The themes'
// drawButtonHints / drawSideButtonHints delegate here, so no screen draws its
// own variant. Overlays never change the underlying content; the next full
// render removes them.
namespace stick_overlay {

// Physical side keys of the X3 in portrait: left edge (BTN_UP, "没啥用") and
// right edge (BTN_DOWN, "有用"), spanning y 155..235.
constexpr int SIDE_KEY_Y = 155;
constexpr int SIDE_KEY_HEIGHT = 80;
// Height of the front-key bar at the bottom edge (themes reserve it as
// buttonHintsHeight).
constexpr int FRONT_BAR_HEIGHT = 64;
// Width a one-glyph side pill occupies from its screen edge (themes reserve it
// as sideButtonHintsWidth).
constexpr int SIDE_HINT_WIDTH = 56;

enum class Bubble : uint8_t { None, Meh, Useful };
constexpr uint32_t BUBBLE_FRAME_MS = 120;
constexpr uint32_t BUBBLE_VISIBLE_MS = 2200;
constexpr uint8_t BUBBLE_FINAL_FRAME = 3;

enum class SideIcon : uint8_t { None, ThumbUp, ThumbDown };

// Outline pills at the side keys with a pointer toward each key. Empty labels
// skip that side. On the X4 both keys sit on the right edge, stacked.
void drawSideKeyHints(const GfxRenderer& renderer, const char* top, const char* bottom,
                      SideIcon topIcon = SideIcon::None, SideIcon bottomIcon = SideIcon::None);

// Card page side hints: thumb-down "没啥用" on the left, "有用" thumb-up on the
// right. Hidden while a feedback window slides over them.
void drawCardSideKeyHints(const GfxRenderer& renderer, bool visible = true);

// White bar along the bottom edge with a top rule; each label sits over its
// physical front key with a small triangle pointing down at it. Empty labels
// leave the slot blank. Draws in the renderer's current orientation (callers
// set portrait).
void drawFrontKeyHints(const GfxRenderer& renderer, const char* back, const char* confirm, const char* left,
                       const char* right);

// Feedback window sliding from its key's edge toward the centre; frame runs
// 0..BUBBLE_FINAL_FRAME.
void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame);

// Lock badge, plus (after a key press while locked) the unlock guide: step
// pills at the side keys (the key to press next is the one filled pill) and a
// prompt in the front bar. Once the left step is done chevrons walk toward the
// right key over Keyguard::CUE_FINAL_FRAME frames.
void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame);

}  // namespace stick_overlay
