#pragma once
#include <cstddef>
#include <cstdint>

// Studio cards are portrait 1-bit frames on the SD card: 528 x 792 pixels,
// 66-byte rows, 1 = black, MSB first. The X3 panel is landscape (792 x 528)
// and the renderer's Portrait orientation draws logical (x, y) at physical
// (y, 527 - x), so a strip of physical columns [x0, x0 + cols) is exactly card
// rows [x0, x0 + cols): streaming a card strip by strip reads the file once,
// front to back.
namespace card_strip {
constexpr uint16_t CARD_WIDTH = 528, CARD_HEIGHT = 792, CARD_ROW_BYTES = CARD_WIDTH / 8;
constexpr size_t GROUP_BYTES = size_t(CARD_ROW_BYTES) * 8;  // eight card rows = one strip byte column

// Writes byte column `byteCol` of a strip (CARD_WIDTH physical rows of
// `stripBytes` bytes) from eight consecutive card rows: physical row p holds
// card column CARD_WIDTH - 1 - p, bit 7 the first of the eight rows, in
// framebuffer polarity (1 = white).
inline void transposeGroup(const uint8_t* rows, uint8_t* strip, uint16_t stripBytes, uint16_t byteCol) {
  for (uint16_t xb = 0; xb < CARD_ROW_BYTES; ++xb) {
    uint8_t in[8];
    for (uint8_t r = 0; r < 8; ++r) in[r] = rows[r * CARD_ROW_BYTES + xb];
    for (uint8_t j = 0; j < 8; ++j) {
      const uint8_t mask = static_cast<uint8_t>(0x80u >> j);
      uint8_t out = 0xFF;
      for (uint8_t r = 0; r < 8; ++r)
        if (in[r] & mask) out = static_cast<uint8_t>(out & ~(0x80u >> r));
      const uint16_t x = static_cast<uint16_t>(xb * 8 + j);
      strip[size_t(CARD_WIDTH - 1 - x) * stripBytes + byteCol] = out;
    }
  }
}
}  // namespace card_strip
