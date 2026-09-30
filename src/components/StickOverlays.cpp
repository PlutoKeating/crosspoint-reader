#include "StickOverlays.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/icons/project_stick_icons.h"
#include "fontIds.h"

namespace stick_overlay {
namespace {

// One chip for every hint: black, rounded, white text, 2-px white halo.
constexpr int CHIP_FONT = NOTOSANSSC_12_FONT_ID;
constexpr int CHIP_HEIGHT = 38;
constexpr int CHIP_RADIUS = 9;
constexpr int CHIP_PAD_X = 14;
constexpr int CHIP_ICON_GAP = 6;
constexpr int HALO = 2;
constexpr int FRONT_BOTTOM_MARGIN = 10;
constexpr int PROMPT_BOTTOM_MARGIN = 10;
constexpr int SIDE_KEY_CENTER = SIDE_KEY_Y + SIDE_KEY_HEIGHT / 2;
constexpr int CUE_STEP = 16;
constexpr int CUE_SIZE = 7;
// Feedback windows: the chips' radius family, one step larger.
constexpr int CARD_RADIUS = 12;
constexpr int CARD_BORDER = 2;
constexpr int CARD_PAD_X = 20;
constexpr int CARD_PAD_Y = 12;

enum class Edge : uint8_t { None, Left, Right };

void drawIcon16(const GfxRenderer& renderer, const uint8_t* bits, int x, int y, bool black, bool flipY = false) {
  for (int row = 0; row < STICK_ICON_SIZE; ++row) {
    const int src = flipY ? STICK_ICON_SIZE - 1 - row : row;
    for (int col = 0; col < STICK_ICON_SIZE; ++col) {
      if (bits[src * STICK_ICON_ROW_BYTES + (col >> 3)] & (0x80 >> (col & 7))) renderer.drawPixel(x + col, y + row, black);
    }
  }
}

int chipWidth(const GfxRenderer& renderer, const char* text, bool icon) {
  const int textWidth = text && *text ? renderer.getTextWidth(CHIP_FONT, text, EpdFontFamily::BOLD) : 0;
  const int iconWidth = icon ? STICK_ICON_SIZE + (textWidth ? CHIP_ICON_GAP : 0) : 0;
  return CHIP_PAD_X * 2 + iconWidth + textWidth;
}

// A chip at (x, y). `edge` flattens the corners that sit against a screen edge
// and drops the halo there. `outlined` draws the white variant (black border).
void drawChip(const GfxRenderer& renderer, int x, int y, int width, const char* text, const uint8_t* icon,
              bool iconFlip, Edge edge, bool outlined, bool iconAfterText = false) {
  const bool roundLeft = edge != Edge::Left;
  const bool roundRight = edge != Edge::Right;
  const int haloLeft = edge == Edge::Left ? 0 : HALO;
  const int haloRight = edge == Edge::Right ? 0 : HALO;
  renderer.fillRoundedRect(x - haloLeft, y - HALO, width + haloLeft + haloRight, CHIP_HEIGHT + HALO * 2,
                           CHIP_RADIUS + HALO, roundLeft, roundRight, roundLeft, roundRight, Color::White);
  renderer.fillRoundedRect(x, y, width, CHIP_HEIGHT, CHIP_RADIUS, roundLeft, roundRight, roundLeft, roundRight,
                           outlined ? Color::White : Color::Black);
  if (outlined)
    renderer.drawRoundedRect(x, y, width, CHIP_HEIGHT, 2, CHIP_RADIUS, roundLeft, roundRight, roundLeft, roundRight,
                             true);
  const bool ink = outlined;  // text/icon colour: black on white, white on black
  const bool hasText = text && *text;
  const int textWidth = hasText ? renderer.getTextWidth(CHIP_FONT, text, EpdFontFamily::BOLD) : 0;
  const int contentWidth = (icon ? STICK_ICON_SIZE : 0) + (icon && hasText ? CHIP_ICON_GAP : 0) + textWidth;
  int cx = x + (width - contentWidth) / 2;
  const int iconY = y + (CHIP_HEIGHT - STICK_ICON_SIZE) / 2;
  if (icon && !iconAfterText) {
    drawIcon16(renderer, icon, cx, iconY, ink, iconFlip);
    cx += STICK_ICON_SIZE + (hasText ? CHIP_ICON_GAP : 0);
  }
  if (hasText) {
    const int lineHeight = renderer.getTextLineHeight(CHIP_FONT, text, EpdFontFamily::BOLD);
    renderer.drawText(CHIP_FONT, cx, y + (CHIP_HEIGHT - lineHeight) / 2, text, ink, EpdFontFamily::BOLD);
    cx += textWidth + (icon ? CHIP_ICON_GAP : 0);
  }
  if (icon && iconAfterText) drawIcon16(renderer, icon, cx, iconY, ink, iconFlip);
}

// Edge tab centred on a side key.
void drawSideTab(const GfxRenderer& renderer, bool left, const char* text, const uint8_t* icon, bool iconFlip,
                 bool outlined, int minWidth = 0) {
  const int width = std::max(minWidth, chipWidth(renderer, text, icon != nullptr));
  const int x = left ? 0 : renderer.getScreenWidth() - width;
  drawChip(renderer, x, SIDE_KEY_CENTER - CHIP_HEIGHT / 2, width, text, icon, iconFlip, left ? Edge::Left : Edge::Right,
           outlined, !left);
}

// Right-pointing chevron centred vertically on cy.
void drawChevron(const GfxRenderer& renderer, int x, int cy) {
  renderer.drawLine(x, cy - CUE_SIZE, x + CUE_SIZE, cy, 3, true);
  renderer.drawLine(x + CUE_SIZE, cy, x, cy + CUE_SIZE, 3, true);
}

}  // namespace

void drawSideKeyHints(const GfxRenderer& renderer, const bool visible) {
  if (!visible) return;
  drawSideTab(renderer, true, tr(STR_PROJECT_STICK_MEH_HINT), StickThumbUp20, true, false);
  drawSideTab(renderer, false, tr(STR_PROJECT_STICK_USEFUL_HINT), StickThumbUp20, false, false);
}

void drawFrontKeyHints(const GfxRenderer& renderer, const char* back, const char* confirm, const char* left,
                       const char* right) {
  if (gpio.hasTouch()) return;
  // Centres of the four physical front keys (same geometry as the theme's
  // button hints).
  constexpr int keyWidth = 106;
  constexpr int x3Keys[] = {38, 154, 268, 384};
  constexpr int x4Keys[] = {25, 130, 245, 350};
  const int* keys = gpio.deviceIsX3() ? x3Keys : x4Keys;
  const char* labels[] = {back, confirm, left, right};
  const int y = renderer.getScreenHeight() - FRONT_BOTTOM_MARGIN - CHIP_HEIGHT;
  for (int i = 0; i < 4; ++i) {
    if (!labels[i] || !*labels[i]) continue;
    const int width = std::min(keyWidth - 4, std::max(chipWidth(renderer, labels[i], false), 76));
    drawChip(renderer, keys[i] + (keyWidth - width) / 2, y, width, labels[i], nullptr, false, Edge::None, false);
  }
}

void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame) {
  if (bubble == Bubble::None) return;
  const char* text = bubble == Bubble::Meh ? tr(STR_PROJECT_STICK_MEH_BUBBLE) : tr(STR_PROJECT_STICK_USEFUL_BUBBLE);
  const int screenWidth = renderer.getScreenWidth();
  const int textWidth = renderer.getTextWidth(NOTOSANSSC_13_FONT_ID, text, EpdFontFamily::BOLD);
  const int textHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, text, EpdFontFamily::BOLD);
  const int cardWidth = std::min(screenWidth - 32, textWidth + STICK_ICON_SIZE + CHIP_ICON_GAP + CARD_PAD_X * 2);
  const int cardHeight = textHeight + CARD_PAD_Y * 2;
  const int finalX = (screenWidth - cardWidth) / 2;
  const bool fromLeft = bubble == Bubble::Meh;
  const int startX = fromLeft ? 0 : screenWidth - cardWidth;
  const int step = std::min<int>(frame, BUBBLE_FINAL_FRAME);
  const int x = startX + (finalX - startX) * step / BUBBLE_FINAL_FRAME;
  const int y = SIDE_KEY_CENTER - cardHeight / 2;
  renderer.fillRoundedRect(x - HALO, y - HALO, cardWidth + HALO * 2, cardHeight + HALO * 2, CARD_RADIUS + HALO,
                           Color::White);
  renderer.fillRoundedRect(x, y, cardWidth, cardHeight, CARD_RADIUS, Color::White);
  renderer.drawRoundedRect(x, y, cardWidth, cardHeight, CARD_BORDER, CARD_RADIUS, true);
  const int contentX = x + (cardWidth - (STICK_ICON_SIZE + CHIP_ICON_GAP + textWidth)) / 2;
  drawIcon16(renderer, StickThumbUp20, contentX, y + (cardHeight - STICK_ICON_SIZE) / 2, true, fromLeft);
  renderer.drawText(NOTOSANSSC_13_FONT_ID, contentX + STICK_ICON_SIZE + CHIP_ICON_GAP, y + CARD_PAD_Y, text, true,
                    EpdFontFamily::BOLD);
}

void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame) {
  using State = project_stick::Keyguard::State;
  // Status lock icon at the top left, on a small white patch.
  constexpr int iconX = 10;
  constexpr int iconY = BaseMetrics::values.topPadding;
  renderer.fillRect(iconX - 2, iconY - 2, STICK_ICON_SIZE + 4, STICK_ICON_SIZE + 4, false);
  drawIcon16(renderer, StickLock20, iconX, iconY, true);
  if (!promptVisible || state == State::Unlocked) return;

  const bool leftDone = state == State::AwaitRight;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  constexpr int stepTabWidth = 56;

  // Step tabs at the side keys: the key to press next is solid black.
  if (leftDone) {
    drawSideTab(renderer, true, nullptr, StickCheck20, false, true, stepTabWidth);
    drawSideTab(renderer, false, "2", nullptr, false, false, stepTabWidth);
    const int laneRight = width - stepTabWidth - 14;
    const int laneLeft = laneRight - CUE_STEP * project_stick::Keyguard::CUE_FINAL_FRAME - CUE_SIZE;
    renderer.fillRect(laneLeft - HALO, SIDE_KEY_CENTER - CUE_SIZE - 4, laneRight - laneLeft + CUE_SIZE + HALO * 2,
                      CUE_SIZE * 2 + 8, false);
    for (int i = 0; i <= cueFrame; ++i) drawChevron(renderer, laneLeft + i * CUE_STEP, SIDE_KEY_CENTER);
  } else {
    drawSideTab(renderer, true, "1", nullptr, false, false, stepTabWidth);
    drawSideTab(renderer, false, "2", nullptr, false, true, stepTabWidth);
  }

  const char* text = leftDone ? tr(STR_KEYGUARD_LEFT_CONFIRMED) : tr(STR_KEYGUARD_LOCKED_PROMPT);
  const uint8_t* icon = leftDone ? StickCheck20 : StickLock20;
  const int promptWidth = std::min(width - 24, chipWidth(renderer, text, true));
  drawChip(renderer, (width - promptWidth) / 2, height - PROMPT_BOTTOM_MARGIN - CHIP_HEIGHT, promptWidth, text, icon,
           false, Edge::None, false);
}

}  // namespace stick_overlay
