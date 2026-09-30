#include "StickOverlays.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/icons/project_stick_icons.h"
#include "fontIds.h"

namespace stick_overlay {
namespace {

constexpr int SIDE_LABEL_GAP = 6;
constexpr int PROMPT_BOTTOM_MARGIN = 8;
constexpr int PROMPT_SIDE_MARGIN = 8;
constexpr int PROMPT_PADDING_X = 16;
constexpr int PROMPT_PADDING_Y = 8;
constexpr int CUE_STEP = 14;
constexpr int CUE_SIZE = 9;

int leftKeyX() { return SIDE_KEY_MARGIN; }
int rightKeyX(const GfxRenderer& renderer) { return renderer.getScreenWidth() - SIDE_KEY_MARGIN - SIDE_KEY_WIDTH; }

void drawThumb(const GfxRenderer& renderer, int x, int y, bool up, bool black) {
  constexpr int size = 24;
  constexpr int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const uint8_t byte = ProjectStickFeedback24Icon.bits[row * rowBytes + (col >> 3)];
      if (((byte >> (7 - (col & 7))) & 1) == 0) renderer.drawPixel(x + col, up ? y + row : y + size - 1 - row, black);
    }
  }
}

// A side-key box: outlined, optionally filled, with either a thumb or a step number.
void drawKeyBox(const GfxRenderer& renderer, int x, bool filled, const char* number, bool thumbUp) {
  renderer.fillRoundedRect(x, SIDE_KEY_Y, SIDE_KEY_WIDTH, SIDE_KEY_HEIGHT, 8, filled ? Color::Black : Color::White);
  renderer.drawRoundedRect(x, SIDE_KEY_Y, SIDE_KEY_WIDTH, SIDE_KEY_HEIGHT, 2, 8, true);
  if (number) {
    const Rect box{x, SIDE_KEY_Y, SIDE_KEY_WIDTH, SIDE_KEY_HEIGHT};
    const int textHeight = renderer.getTextLineHeight(UI_10_FONT_ID, number);
    UITheme::drawCenteredText(renderer, box, UI_10_FONT_ID, SIDE_KEY_Y + (SIDE_KEY_HEIGHT - textHeight) / 2, number,
                              !filled, EpdFontFamily::BOLD);
    return;
  }
  drawThumb(renderer, x + (SIDE_KEY_WIDTH - 24) / 2, SIDE_KEY_Y + (SIDE_KEY_HEIGHT - 24) / 2, thumbUp, !filled);
}

void drawSideLabel(const GfxRenderer& renderer, const char* text, bool left) {
  const int width = renderer.getTextWidth(SMALL_FONT_ID, text) + 12;
  const int height = renderer.getTextLineHeight(SMALL_FONT_ID, text) + 6;
  const int x = left ? leftKeyX() + SIDE_KEY_WIDTH + SIDE_LABEL_GAP : rightKeyX(renderer) - SIDE_LABEL_GAP - width;
  const int y = SIDE_KEY_Y + (SIDE_KEY_HEIGHT - height) / 2;
  renderer.fillRoundedRect(x, y, width, height, height / 2, Color::White);
  renderer.drawRoundedRect(x, y, width, height, 1, height / 2, true);
  renderer.drawText(SMALL_FONT_ID, x + 6, y + 3, text);
}

// Right-pointing chevron centred vertically on cy.
void drawChevron(const GfxRenderer& renderer, int x, int cy) {
  renderer.drawLine(x, cy - CUE_SIZE, x + CUE_SIZE, cy, 3, true);
  renderer.drawLine(x + CUE_SIZE, cy, x, cy + CUE_SIZE, 3, true);
}

}  // namespace

void drawSideKeyHints(const GfxRenderer& renderer, const bool labels) {
  drawKeyBox(renderer, leftKeyX(), false, nullptr, false);
  drawKeyBox(renderer, rightKeyX(renderer), false, nullptr, true);
  if (!labels) return;
  drawSideLabel(renderer, tr(STR_PROJECT_STICK_MEH_HINT), true);
  drawSideLabel(renderer, tr(STR_PROJECT_STICK_USEFUL_HINT), false);
}

void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame) {
  if (bubble == Bubble::None) return;
  const char* text = bubble == Bubble::Meh ? tr(STR_PROJECT_STICK_MEH_BUBBLE) : tr(STR_PROJECT_STICK_USEFUL_BUBBLE);
  constexpr int horizontalPadding = 18;
  constexpr int verticalPadding = 11;
  constexpr int sideGap = 8;
  constexpr int borderWidth = 2;
  constexpr int cornerRadius = 10;
  const int screenWidth = renderer.getScreenWidth();
  const int textWidth = renderer.getTextWidth(NOTOSANSSC_13_FONT_ID, text, EpdFontFamily::BOLD);
  const int textHeight = renderer.getTextLineHeight(NOTOSANSSC_13_FONT_ID, text);
  const int maxWidth = screenWidth - 2 * (SIDE_KEY_MARGIN + SIDE_KEY_WIDTH + sideGap);
  const int bubbleWidth = std::min(maxWidth, textWidth + horizontalPadding * 2);
  const int bubbleHeight = textHeight + verticalPadding * 2;
  const int finalX = (screenWidth - bubbleWidth) / 2;
  const bool fromLeft = bubble == Bubble::Meh;
  const int startX = fromLeft ? SIDE_KEY_MARGIN + SIDE_KEY_WIDTH + sideGap
                              : screenWidth - SIDE_KEY_MARGIN - SIDE_KEY_WIDTH - sideGap - bubbleWidth;
  const int step = std::min<int>(frame, BUBBLE_FINAL_FRAME);
  const int x = startX + (finalX - startX) * step / BUBBLE_FINAL_FRAME;
  const int y = SIDE_KEY_Y + (SIDE_KEY_HEIGHT - bubbleHeight) / 2;
  renderer.fillRoundedRect(x, y, bubbleWidth, bubbleHeight, cornerRadius, Color::White);
  renderer.drawRoundedRect(x, y, bubbleWidth, bubbleHeight, borderWidth, cornerRadius, true);
  UITheme::drawCenteredText(renderer, Rect{x, y, bubbleWidth, bubbleHeight}, NOTOSANSSC_13_FONT_ID, y + verticalPadding,
                            text, true, EpdFontFamily::BOLD);
}

void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame) {
  using State = project_stick::Keyguard::State;
  constexpr int iconHeight = BaseMetrics::values.batteryHeight;
  constexpr int iconWidth = 12;
  constexpr int iconX = 12;
  constexpr int iconY = BaseMetrics::values.topPadding;
  renderer.fillRect(iconX, iconY, iconWidth, iconHeight, false);
  renderer.drawRoundedRect(iconX + 3, iconY, iconWidth - 6, 8, 1, 3, true);
  renderer.fillRoundedRect(iconX, iconY + 5, iconWidth, iconHeight - 5, 2, Color::Black);
  renderer.fillRect(iconX + iconWidth / 2, iconY + 8, 1, 3, false);
  if (!promptVisible || state == State::Unlocked) return;

  const bool leftDone = state == State::AwaitRight;
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();

  // Which keys to press, in order. After the left step, the right key is
  // filled and a cue slides toward it.
  drawKeyBox(renderer, leftKeyX(), false, leftDone ? "" : "1", false);
  if (leftDone) {
    // Check mark: the left step is done (drawn, as UI fonts lack the glyph).
    const int cx = leftKeyX() + SIDE_KEY_WIDTH / 2;
    const int cy = SIDE_KEY_Y + SIDE_KEY_HEIGHT / 2;
    renderer.drawLine(cx - 7, cy, cx - 2, cy + 6, 3, true);
    renderer.drawLine(cx - 2, cy + 6, cx + 8, cy - 7, 3, true);
  }
  drawKeyBox(renderer, rightKeyX(renderer), leftDone, "2", true);
  const int cueY = SIDE_KEY_Y + SIDE_KEY_HEIGHT / 2;
  const int laneRight = rightKeyX(renderer) - SIDE_LABEL_GAP;
  const int laneLeft = laneRight - CUE_STEP * (project_stick::Keyguard::CUE_FINAL_FRAME + 1) - CUE_SIZE;
  renderer.fillRect(laneLeft, cueY - CUE_SIZE - 3, laneRight - laneLeft, CUE_SIZE * 2 + 7, false);
  if (leftDone) {
    for (int i = 0; i <= cueFrame; ++i) drawChevron(renderer, laneLeft + i * CUE_STEP, cueY);
  }

  const char* text = leftDone ? tr(STR_KEYGUARD_LEFT_CONFIRMED) : tr(STR_KEYGUARD_LOCKED_PROMPT);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  const int textHeight = renderer.getTextLineHeight(UI_10_FONT_ID, text, EpdFontFamily::BOLD);
  const int promptWidth = std::min(width - 2 * PROMPT_SIDE_MARGIN, textWidth + 2 * PROMPT_PADDING_X);
  const int promptHeight = textHeight + 2 * PROMPT_PADDING_Y;
  const int promptX = (width - promptWidth) / 2;
  const int promptY = height - promptHeight - PROMPT_BOTTOM_MARGIN;
  renderer.fillRoundedRect(promptX, promptY, promptWidth, promptHeight, 10, leftDone ? Color::Black : Color::White);
  renderer.drawRoundedRect(promptX, promptY, promptWidth, promptHeight, 2, 10, true);
  UITheme::drawCenteredText(renderer, Rect{promptX, promptY, promptWidth, promptHeight}, UI_10_FONT_ID,
                            promptY + PROMPT_PADDING_Y, text, !leftDone, EpdFontFamily::BOLD);
}

}  // namespace stick_overlay
