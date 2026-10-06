#include <CardStrip.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace {
using namespace card_strip;
constexpr int PANEL_W = CARD_HEIGHT, PANEL_H = CARD_WIDTH, PANEL_ROW = PANEL_W / 8;

std::vector<uint8_t> randomCard(uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<uint8_t> card(size_t(CARD_ROW_BYTES) * CARD_HEIGHT);
  for (auto& b : card) b = static_cast<uint8_t>(rng());
  return card;
}

// What StudioFrame::render() leaves in the framebuffer: drawPixel(x, y) for
// every black card pixel under Portrait (physical (y, 527 - x), 0 = black).
std::vector<uint8_t> reference(const std::vector<uint8_t>& card) {
  std::vector<uint8_t> fb(size_t(PANEL_ROW) * PANEL_H, 0xFF);
  for (int y = 0; y < CARD_HEIGHT; ++y)
    for (int x = 0; x < CARD_WIDTH; ++x)
      if (card[size_t(y) * CARD_ROW_BYTES + x / 8] & (0x80 >> (x & 7))) {
        const int px = y, py = CARD_WIDTH - 1 - x;
        fb[size_t(py) * PANEL_ROW + px / 8] &= static_cast<uint8_t>(~(0x80 >> (px & 7)));
      }
  return fb;
}

// The streamed path: strips of `cols` panel columns, each from card rows
// [x0, x0 + cols) read front to back, placed the way the X3 PTL windows land.
std::vector<uint8_t> streamed(const std::vector<uint8_t>& card, uint16_t stripCols) {
  std::vector<uint8_t> fb(size_t(PANEL_ROW) * PANEL_H, 0x00);
  std::vector<uint8_t> strip(size_t(PANEL_H) * stripCols / 8);
  for (int x0 = 0; x0 < PANEL_W; x0 += stripCols) {
    const int cols = std::min<int>(stripCols, PANEL_W - x0);
    const uint16_t bytes = static_cast<uint16_t>(cols / 8);
    for (uint16_t k = 0; k < bytes; ++k)
      transposeGroup(card.data() + size_t(x0 + 8 * k) * CARD_ROW_BYTES, strip.data(), bytes, k);
    for (int py = 0; py < PANEL_H; ++py)
      std::memcpy(fb.data() + size_t(py) * PANEL_ROW + x0 / 8, strip.data() + size_t(py) * bytes, bytes);
  }
  return fb;
}
}  // namespace

TEST(CardStrip, StreamedStripsMatchTheFramebufferRender) {
  for (const uint16_t cols : {uint16_t(72), uint16_t(24), uint16_t(8), uint16_t(48)}) {
    const auto card = randomCard(cols);
    EXPECT_EQ(streamed(card, cols), reference(card)) << "strip width " << cols;
  }
}

TEST(CardStrip, CornersLandWherePortraitPutsThem) {
  std::vector<uint8_t> card(size_t(CARD_ROW_BYTES) * CARD_HEIGHT, 0);
  card[0] = 0x80;                                                     // logical (0, 0)
  card[size_t(CARD_HEIGHT - 1) * CARD_ROW_BYTES + CARD_ROW_BYTES - 1] = 0x01;  // logical (527, 791)
  const auto fb = streamed(card, 72);
  // (0, 0) -> physical (0, 527); (527, 791) -> physical (791, 0).
  EXPECT_EQ(fb[size_t(PANEL_H - 1) * PANEL_ROW + 0], 0x7F);
  EXPECT_EQ(fb[PANEL_ROW - 1], 0xFE);
  size_t black = 0;
  for (const uint8_t b : fb) black += 8 - __builtin_popcount(b);
  EXPECT_EQ(black, 2u);
}
