#include "StickOverlays.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/UITheme.h"
#include "components/icons/project_stick_icons.h"
#include "fontIds.h"

namespace stick_overlay {
namespace {

// Hint labels use Noto Sans SC 10 (Noto Sans Latin/Greek/Cyrillic plus CJK, a
// ~20 px line); feedback windows one step larger.
constexpr int LABEL_FONT = NOTOSANSSC_10_FONT_ID;
constexpr int BUBBLE_FONT = NOTOSANSSC_12_FONT_ID;
constexpr int STROKE = 2;
// Side pills.
constexpr int PILL_HEIGHT = 40;
constexpr int PILL_RADIUS = PILL_HEIGHT / 2;
constexpr int PILL_PAD = 12;
constexpr int PILL_ICON_GAP = 7;
constexpr int PILL_EDGE_INSET = 10;
constexpr int PILL_MAX_WIDTH = 220;
static_assert(PILL_EDGE_INSET + PILL_HEIGHT <= SIDE_HINT_WIDTH);
constexpr int POINTER_DEPTH = 7;
constexpr int POINTER_HALF = 7;
// Front bar.
constexpr int BAR_LABEL_TOP = 14;
constexpr int BAR_SLOT_WIDTH = 108;
constexpr int BAR_TRIANGLE_HALF = 6;
constexpr int BAR_TRIANGLE_HEIGHT = 7;
constexpr int BAR_TRIANGLE_BOTTOM = 7;
// Unlock cue chevrons, left of the right pill.
constexpr int CUE_STEP = 14;
constexpr int CUE_SIZE = 6;
constexpr int CUE_GAP = 10;
// Feedback windows.
constexpr int CARD_RADIUS = 16;
constexpr int CARD_PAD_X = 22;
constexpr int CARD_PAD_Y = 14;
constexpr int HALO = 2;
// Status notice above the front bar.
constexpr int NOTICE_MARGIN = 24;
constexpr int NOTICE_BOTTOM_GAP = 14;
constexpr int NOTICE_DOT = 10;
constexpr int NOTICE_DOT_GAP = 6;
constexpr int NOTICE_BAR_GAP = 10;
constexpr int NOTICE_BAR_HEIGHT = 12;
// X4 stacks both side keys on the right edge.
constexpr int X4_SIDE_KEY_Y = 345;

bool hasText(const char* text) { return text && *text; }

void drawIcon(const GfxRenderer& renderer, const uint8_t* bits, int x, int y, bool black, bool flipY = false) {
  for (int row = 0; row < STICK_ICON_SIZE; ++row) {
    const int src = flipY ? STICK_ICON_SIZE - 1 - row : row;
    for (int col = 0; col < STICK_ICON_SIZE; ++col) {
      if (bits[src * STICK_ICON_ROW_BYTES + (col >> 3)] & (0x80 >> (col & 7)))
        renderer.drawPixel(x + col, y + row, black);
    }
  }
}

void drawSideIcon(const GfxRenderer& renderer, SideIcon icon, int x, int y, bool black) {
  if (icon == SideIcon::None) return;
  drawIcon(renderer, StickThumbUp20, x, y, black, icon == SideIcon::ThumbDown);
}

// Solid triangle with its tip at (tipX, tipY), pointing left, right or down.
enum class Point : uint8_t { Left, Right, Down };
void drawPointer(const GfxRenderer& renderer, int tipX, int tipY, Point dir, int half, int depth) {
  int xs[3], ys[3];
  if (dir == Point::Down) {
    xs[0] = tipX - half, ys[0] = tipY - depth;
    xs[1] = tipX + half, ys[1] = tipY - depth;
  } else {
    const int base = dir == Point::Left ? tipX + depth : tipX - depth;
    xs[0] = base, ys[0] = tipY - half;
    xs[1] = base, ys[1] = tipY + half;
  }
  xs[2] = tipX, ys[2] = tipY;
  renderer.fillPolygon(xs, ys, 3, true);
}

struct Pill {
  const char* text;
  const uint8_t* icon;
  bool iconFlip;
  bool filled;  // the one emphasised element: black fill, white ink
};

int pillWidth(const GfxRenderer& renderer, const std::string& text, bool icon) {
  const int textWidth = text.empty() ? 0 : renderer.getTextWidth(LABEL_FONT, text.c_str());
  const int iconWidth = icon ? STICK_ICON_SIZE + (textWidth ? PILL_ICON_GAP : 0) : 0;
  return PILL_PAD * 2 + iconWidth + textWidth;
}

// A pill whose vertical centre is cy, against the left or right edge, with the
// icon on the outer side and a pointer from the outer end toward the key.
// Returns the pill's x range through `outX`/`outWidth`.
void drawSidePill(const GfxRenderer& renderer, bool left, int cy, const Pill& pill, int* outX = nullptr,
                  int* outWidth = nullptr) {
  const std::string text =
      hasText(pill.text) ? renderer.truncatedText(LABEL_FONT, pill.text, PILL_MAX_WIDTH - PILL_PAD * 2) : "";
  const bool icon = pill.icon != nullptr;
  const int width = std::max(PILL_HEIGHT, pillWidth(renderer, text, icon));
  const int x = left ? PILL_EDGE_INSET : renderer.getScreenWidth() - PILL_EDGE_INSET - width;
  const int y = cy - PILL_HEIGHT / 2;
  // White halo keeps the outline legible over any card.
  renderer.fillRoundedRect(x - HALO, y - HALO, width + HALO * 2, PILL_HEIGHT + HALO * 2, PILL_RADIUS + HALO,
                           Color::White);
  renderer.fillRoundedRect(x, y, width, PILL_HEIGHT, PILL_RADIUS, pill.filled ? Color::Black : Color::White);
  renderer.drawRoundedRect(x, y, width, PILL_HEIGHT, STROKE, PILL_RADIUS, true);
  const int pointerTip = left ? x - POINTER_DEPTH - 1 : x + width + POINTER_DEPTH;
  renderer.fillRect(left ? pointerTip : x + width, cy - POINTER_HALF - HALO, POINTER_DEPTH + 1,
                    POINTER_HALF * 2 + HALO * 2 + 1, false);
  drawPointer(renderer, pointerTip, cy, left ? Point::Left : Point::Right, POINTER_HALF, POINTER_DEPTH + 1);

  const bool ink = !pill.filled;
  const int textWidth = text.empty() ? 0 : renderer.getTextWidth(LABEL_FONT, text.c_str());
  const int contentWidth = (icon ? STICK_ICON_SIZE : 0) + (icon && textWidth ? PILL_ICON_GAP : 0) + textWidth;
  int cx = x + (width - contentWidth) / 2;
  const int iconY = cy - STICK_ICON_SIZE / 2;
  const auto drawLabel = [&]() {
    if (text.empty()) return;
    const int lineHeight = renderer.getTextLineHeight(LABEL_FONT, text.c_str());
    renderer.drawText(LABEL_FONT, cx, cy - lineHeight / 2, text.c_str(), ink);
    cx += textWidth + PILL_ICON_GAP;
  };
  if (left) {
    if (icon) {
      drawIcon(renderer, pill.icon, cx, iconY, ink, pill.iconFlip);
      cx += STICK_ICON_SIZE + PILL_ICON_GAP;
    }
    drawLabel();
  } else {
    drawLabel();
    if (icon) drawIcon(renderer, pill.icon, cx, iconY, ink, pill.iconFlip);
  }
  if (outX) *outX = x;
  if (outWidth) *outWidth = width;
}

const uint8_t* iconBits(SideIcon icon) { return icon == SideIcon::None ? nullptr : StickThumbUp20; }

// Physical front key centres in portrait (106-px keys).
const int* frontKeyCentres() {
  static constexpr int x3[] = {91, 207, 321, 437};
  static constexpr int x4[] = {78, 183, 298, 403};
  return gpio.deviceIsX3() ? x3 : x4;
}

// White bar over the bottom edge with a 2-px rule on top.
int drawFrontBar(const GfxRenderer& renderer) {
  const int top = renderer.getScreenHeight() - FRONT_BAR_HEIGHT;
  renderer.fillRect(0, top, renderer.getScreenWidth(), FRONT_BAR_HEIGHT, false);
  renderer.fillRect(0, top, renderer.getScreenWidth(), STROKE, true);
  return top;
}

// Right-pointing outline chevron centred vertically on cy.
void drawChevron(const GfxRenderer& renderer, int x, int cy) {
  renderer.drawLine(x, cy - CUE_SIZE, x + CUE_SIZE, cy, STROKE, true);
  renderer.drawLine(x + CUE_SIZE, cy, x, cy + CUE_SIZE, STROKE, true);
}

}  // namespace

void drawSideKeyHints(const GfxRenderer& renderer, const char* top, const char* bottom, const SideIcon topIcon,
                      const SideIcon bottomIcon) {
  if (gpio.hasTouch()) return;
  const bool x3 = gpio.deviceIsX3();
  const int topCentre = (x3 ? SIDE_KEY_Y : X4_SIDE_KEY_Y) + SIDE_KEY_HEIGHT / 2;
  const int bottomCentre = x3 ? topCentre : topCentre + SIDE_KEY_HEIGHT;
  if (hasText(top) || topIcon != SideIcon::None)
    drawSidePill(renderer, x3, topCentre, {top, iconBits(topIcon), topIcon == SideIcon::ThumbDown, false});
  if (hasText(bottom) || bottomIcon != SideIcon::None)
    drawSidePill(renderer, false, bottomCentre,
                 {bottom, iconBits(bottomIcon), bottomIcon == SideIcon::ThumbDown, false});
}

void drawCardSideKeyHints(const GfxRenderer& renderer, const bool visible) {
  if (!visible) return;
  drawSideKeyHints(renderer, tr(STR_PROJECT_STICK_MEH_HINT), tr(STR_PROJECT_STICK_USEFUL_HINT), SideIcon::ThumbDown,
                   SideIcon::ThumbUp);
}

void drawFrontKeyHints(const GfxRenderer& renderer, const char* back, const char* confirm, const char* left,
                       const char* right) {
  if (gpio.hasTouch()) return;
  const char* labels[] = {back, confirm, left, right};
  if (std::none_of(labels, labels + 4, hasText)) return;
  const int top = drawFrontBar(renderer);
  const int* centres = frontKeyCentres();
  const int bottom = renderer.getScreenHeight();
  for (int i = 0; i < 4; ++i) {
    if (!hasText(labels[i])) continue;
    const std::string text = renderer.truncatedText(LABEL_FONT, labels[i], BAR_SLOT_WIDTH);
    const int width = renderer.getTextWidth(LABEL_FONT, text.c_str());
    renderer.drawText(LABEL_FONT, centres[i] - width / 2, top + BAR_LABEL_TOP, text.c_str(), true);
    drawPointer(renderer, centres[i], bottom - BAR_TRIANGLE_BOTTOM, Point::Down, BAR_TRIANGLE_HALF,
                BAR_TRIANGLE_HEIGHT);
  }
}

void drawFeedbackBubble(const GfxRenderer& renderer, Bubble bubble, uint8_t frame) {
  if (bubble == Bubble::None) return;
  const char* text = bubble == Bubble::Meh ? tr(STR_PROJECT_STICK_MEH_BUBBLE) : tr(STR_PROJECT_STICK_USEFUL_BUBBLE);
  const int screenWidth = renderer.getScreenWidth();
  const int textWidth = renderer.getTextWidth(BUBBLE_FONT, text);
  const int textHeight = renderer.getTextLineHeight(BUBBLE_FONT, text);
  const int cardWidth = std::min(screenWidth - 32, textWidth + STICK_ICON_SIZE + PILL_ICON_GAP + CARD_PAD_X * 2);
  const int cardHeight = textHeight + CARD_PAD_Y * 2;
  const int finalX = (screenWidth - cardWidth) / 2;
  const bool fromLeft = bubble == Bubble::Meh;
  const int startX = fromLeft ? PILL_EDGE_INSET : screenWidth - PILL_EDGE_INSET - cardWidth;
  const int step = std::min<int>(frame, BUBBLE_FINAL_FRAME);
  const int x = startX + (finalX - startX) * step / BUBBLE_FINAL_FRAME;
  const int y = SIDE_KEY_Y + SIDE_KEY_HEIGHT / 2 - cardHeight / 2;
  renderer.fillRoundedRect(x - HALO, y - HALO, cardWidth + HALO * 2, cardHeight + HALO * 2, CARD_RADIUS + HALO,
                           Color::White);
  renderer.drawRoundedRect(x, y, cardWidth, cardHeight, STROKE, CARD_RADIUS, true);
  const int contentX = x + (cardWidth - (STICK_ICON_SIZE + PILL_ICON_GAP + textWidth)) / 2;
  drawIcon(renderer, StickThumbUp20, contentX, y + (cardHeight - STICK_ICON_SIZE) / 2, true, fromLeft);
  renderer.drawText(BUBBLE_FONT, contentX + STICK_ICON_SIZE + PILL_ICON_GAP, y + CARD_PAD_Y, text, true);
}

bool noticeBusy(const Notice notice) {
  switch (notice) {
    case Notice::Receiving:
    case Notice::Refreshing:
    case Notice::Syncing:
    case Notice::WifiConnecting:
    case Notice::WifiScanning:
      return true;
    default:
      return false;
  }
}

void drawNotice(const GfxRenderer& renderer, const Notice notice, const int percent, const uint8_t frame) {
  if (notice == Notice::None) return;
  char buffer[64];
  const char* text = "";
  switch (notice) {
    case Notice::PhoneConnected:
      text = tr(STR_LINK_PHONE_CONNECTED);
      break;
    case Notice::Receiving:
      snprintf(buffer, sizeof(buffer), tr(STR_LINK_RECEIVING), std::clamp(percent, 0, 100));
      text = buffer;
      break;
    case Notice::Refreshing:
      text = tr(STR_LINK_REFRESHING);
      break;
    case Notice::Done:
      text = tr(STR_LINK_DONE);
      break;
    case Notice::Failed:
      text = tr(STR_LINK_FAILED);
      break;
    case Notice::Syncing:
      text = tr(STR_LINK_SYNCING);
      break;
    case Notice::Synced:
      text = tr(STR_LINK_SYNCED);
      break;
    case Notice::SyncFailed:
      text = tr(STR_LINK_SYNC_FAILED);
      break;
    case Notice::WifiConnecting:
      text = tr(STR_PROJECT_STICK_WIFI_CONNECTING);
      break;
    case Notice::WifiConnected:
      text = tr(STR_PROJECT_STICK_WIFI_CONNECTED);
      break;
    case Notice::WifiFailed:
      text = tr(STR_PROJECT_STICK_WIFI_FAILED);
      break;
    case Notice::WifiScanning:
      text = tr(STR_LINK_WIFI_SCANNING);
      break;
    case Notice::None:
      return;
  }
  const bool busy = noticeBusy(notice);
  const bool done = notice == Notice::Done || notice == Notice::Synced || notice == Notice::WifiConnected ||
                    notice == Notice::PhoneConnected;
  const bool bar = notice == Notice::Receiving;
  const int screenWidth = renderer.getScreenWidth();
  const int maxWidth = screenWidth - NOTICE_MARGIN * 2;
  const int leadWidth = busy ? NOTICE_DOT * 3 + NOTICE_DOT_GAP * 2 : done ? STICK_ICON_SIZE : 0;
  const int leadGap = leadWidth ? PILL_ICON_GAP + 3 : 0;
  const std::string fitted = renderer.truncatedText(BUBBLE_FONT, text, maxWidth - CARD_PAD_X * 2 - leadWidth - leadGap);
  const int textWidth = renderer.getTextWidth(BUBBLE_FONT, fitted.c_str());
  const int textHeight = renderer.getTextLineHeight(BUBBLE_FONT, fitted.c_str());
  const int contentWidth = leadWidth + leadGap + textWidth;
  // A transfer keeps one width for its whole run, so the window does not
  // jitter as the percentage grows.
  const int cardWidth = bar ? maxWidth : std::min(maxWidth, contentWidth + CARD_PAD_X * 2);
  const int cardHeight = textHeight + CARD_PAD_Y * 2 + (bar ? NOTICE_BAR_GAP + NOTICE_BAR_HEIGHT : 0);
  const int x = (screenWidth - cardWidth) / 2;
  const int y = renderer.getScreenHeight() - FRONT_BAR_HEIGHT - NOTICE_BOTTOM_GAP - cardHeight;
  renderer.fillRoundedRect(x - HALO, y - HALO, cardWidth + HALO * 2, cardHeight + HALO * 2, CARD_RADIUS + HALO,
                           Color::White);
  renderer.drawRoundedRect(x, y, cardWidth, cardHeight, STROKE, CARD_RADIUS, true);

  int cx = x + (cardWidth - contentWidth) / 2;
  const int textCentre = y + CARD_PAD_Y + textHeight / 2;
  if (busy) {
    // Three dots; the filled one walks left to right with each frame.
    for (int i = 0; i < 3; ++i) {
      const int dotX = cx + i * (NOTICE_DOT + NOTICE_DOT_GAP);
      const int dotY = textCentre - NOTICE_DOT / 2;
      if (i == frame % 3)
        renderer.fillRoundedRect(dotX, dotY, NOTICE_DOT, NOTICE_DOT, NOTICE_DOT / 2, Color::Black);
      else
        renderer.drawRoundedRect(dotX, dotY, NOTICE_DOT, NOTICE_DOT, 1, NOTICE_DOT / 2, true);
    }
  } else if (done) {
    drawIcon(renderer, StickCheck20, cx, textCentre - STICK_ICON_SIZE / 2, true);
  }
  cx += leadWidth + leadGap;
  renderer.drawText(BUBBLE_FONT, cx, y + CARD_PAD_Y, fitted.c_str(), true);

  if (bar) {
    const int barX = x + CARD_PAD_X;
    const int barY = y + CARD_PAD_Y + textHeight + NOTICE_BAR_GAP;
    const int barWidth = cardWidth - CARD_PAD_X * 2;
    renderer.drawRoundedRect(barX, barY, barWidth, NOTICE_BAR_HEIGHT, 1, NOTICE_BAR_HEIGHT / 2, true);
    const int fill = (barWidth - 4) * std::clamp(percent, 0, 100) / 100;
    if (fill > 0)
      renderer.fillRoundedRect(barX + 2, barY + 2, std::max(fill, NOTICE_BAR_HEIGHT - 4), NOTICE_BAR_HEIGHT - 4,
                               (NOTICE_BAR_HEIGHT - 4) / 2, Color::Black);
  }
}

void drawKeyguard(const GfxRenderer& renderer, project_stick::Keyguard::State state, bool promptVisible,
                  uint8_t cueFrame) {
  using State = project_stick::Keyguard::State;
  // Lock badge at the top left: the icon in a small outline circle.
  constexpr int badge = 34;
  constexpr int badgeX = PILL_EDGE_INSET;
  constexpr int badgeY = BaseMetrics::values.topPadding + 2;
  renderer.fillRoundedRect(badgeX - HALO, badgeY - HALO, badge + HALO * 2, badge + HALO * 2, badge / 2 + HALO,
                           Color::White);
  renderer.drawRoundedRect(badgeX, badgeY, badge, badge, STROKE, badge / 2, true);
  drawIcon(renderer, StickLock20, badgeX + (badge - STICK_ICON_SIZE) / 2, badgeY + (badge - STICK_ICON_SIZE) / 2, true);
  if (!promptVisible || state == State::Unlocked) return;

  const bool leftDone = state == State::AwaitRight;
  const int cy = SIDE_KEY_Y + SIDE_KEY_HEIGHT / 2;
  if (leftDone) {
    drawSidePill(renderer, true, cy, {tr(STR_KEYGUARD_STEP_DONE), StickCheck20, false, false});
    int rightX = 0;
    drawSidePill(renderer, false, cy, {tr(STR_KEYGUARD_STEP_SECOND), nullptr, false, true}, &rightX);
    const int laneRight = rightX - CUE_GAP - CUE_SIZE;
    const int laneLeft = laneRight - CUE_STEP * project_stick::Keyguard::CUE_FINAL_FRAME;
    renderer.fillRect(laneLeft - HALO * 2, cy - CUE_SIZE - HALO * 2, laneRight - laneLeft + CUE_SIZE + HALO * 4,
                      CUE_SIZE * 2 + HALO * 4, false);
    for (int i = 0; i <= cueFrame; ++i) drawChevron(renderer, laneLeft + i * CUE_STEP, cy);
  } else {
    drawSidePill(renderer, true, cy, {tr(STR_KEYGUARD_STEP_FIRST), nullptr, false, true});
    drawSidePill(renderer, false, cy, {tr(STR_KEYGUARD_STEP_SECOND), nullptr, false, false});
  }

  // The prompt takes the front bar's place.
  const int top = drawFrontBar(renderer);
  const char* text = leftDone ? tr(STR_KEYGUARD_LEFT_CONFIRMED) : tr(STR_KEYGUARD_LOCKED_PROMPT);
  const uint8_t* icon = leftDone ? StickCheck20 : StickLock20;
  const int width = renderer.getScreenWidth();
  const std::string fitted =
      renderer.truncatedText(LABEL_FONT, text, width - PILL_EDGE_INSET * 2 - STICK_ICON_SIZE - PILL_ICON_GAP);
  const int textWidth = renderer.getTextWidth(LABEL_FONT, fitted.c_str());
  const int lineHeight = renderer.getTextLineHeight(LABEL_FONT, fitted.c_str());
  const int barCentre = top + STROKE + (FRONT_BAR_HEIGHT - STROKE) / 2;
  const int x = (width - STICK_ICON_SIZE - PILL_ICON_GAP - textWidth) / 2;
  drawIcon(renderer, icon, x, barCentre - STICK_ICON_SIZE / 2, true);
  renderer.drawText(LABEL_FONT, x + STICK_ICON_SIZE + PILL_ICON_GAP, barCentre - lineHeight / 2, fitted.c_str(), true);
}

}  // namespace stick_overlay
