#include <FrameSlots.h>
#include <gtest/gtest.h>

using namespace frame_slots;

namespace {
Digest digestOf(uint8_t n) {
  Digest d{};
  d.fill(n);
  return d;
}
}  // namespace

TEST(FrameSlots, LayoutIsSectorAligned) {
  EXPECT_EQ(SLOT_BYTES % 512, 0u);
  EXPECT_GE(SLOT_BYTES, FRAME_BYTES);
  EXPECT_LT(SLOT_BYTES - FRAME_BYTES, 512u);
  EXPECT_GE(SLOTS, 64);
  EXPECT_LE(SLOTS, 128);
  EXPECT_EQ(framesIn(8 + 2000 + 17 * FRAME_BYTES), 17u);
  EXPECT_EQ(framesIn(FRAME_BYTES), 1u);
}

TEST(FrameSlots, FreeSlotsFirstThenOldestUnpinned) {
  Index index;
  Pins pins;
  for (uint8_t i = 0; i < SLOTS; ++i) {
    const uint16_t s = index.pick(pins);
    ASSERT_NE(s, NO_SLOT);
    EXPECT_FALSE(index.entry(s).valid);
    index.store(s, digestOf(i));
  }
  // Full: the oldest frame goes first, unless a kept program pins it.
  EXPECT_EQ(index.pick(pins), index.find(digestOf(0)));
  pins.set(index.find(digestOf(0)));
  EXPECT_EQ(index.pick(pins), index.find(digestOf(1)));
  EXPECT_EQ(index.available(pins), size_t(SLOTS - 1));
  pins.set();
  EXPECT_EQ(index.pick(pins), NO_SLOT);
  EXPECT_EQ(index.available(pins), 0u);
}

TEST(FrameSlots, ReleasedSlotHoldsNothing) {
  Index index;
  index.store(5, digestOf(7));
  EXPECT_EQ(index.find(digestOf(7)), 5);
  index.release(5);
  EXPECT_EQ(index.find(digestOf(7)), NO_SLOT);
  EXPECT_EQ(index.pick(Pins{}), 0);
}

TEST(FrameSlots, IndexRoundTripKeepsStampOrder) {
  Index index;
  index.store(3, digestOf(1));
  index.store(9, digestOf(2));
  index.store(1, digestOf(3));
  std::vector<uint8_t> bytes(INDEX_BYTES);
  index.serialize(bytes.data());
  Index back;
  ASSERT_TRUE(back.parse(bytes.data(), bytes.size()));
  EXPECT_EQ(back.find(digestOf(2)), 9);
  // Fill the free slots, then the oldest (slot 3) is evicted first.
  Pins pins;
  for (int i = 0; i < SLOTS - 3; ++i) back.store(back.pick(pins), digestOf(100 + i % 100));
  EXPECT_EQ(back.pick(pins), 3);
  // One entry written alone matches the full serialization.
  uint8_t entry[ENTRY_BYTES];
  index.serializeEntry(9, entry);
  EXPECT_EQ(memcmp(entry, bytes.data() + Index::entryOffset(9), ENTRY_BYTES), 0);
  bytes[0] = 'X';
  EXPECT_FALSE(back.parse(bytes.data(), bytes.size()));
  EXPECT_FALSE(back.parse(bytes.data(), bytes.size() - 1));
}

TEST(FrameSlots, RecordHeader) {
  Record r;
  r.count = 17;
  r.headerBytes = 2331;
  r.done = 4;
  uint8_t bytes[RECORD_HEADER];
  encodeRecordHeader(r, bytes);
  Record back;
  ASSERT_TRUE(parseRecordHeader(bytes, back));
  EXPECT_EQ(back.count, 17);
  EXPECT_EQ(back.headerBytes, 2331u);
  EXPECT_EQ(back.done, 4u);
  EXPECT_EQ(back.contentBytes(), 2331u + 17u * FRAME_BYTES);
  EXPECT_EQ(back.headerOffset(), RECORD_HEADER + 34);
  r.done = 18;  // more frames done than the record has
  encodeRecordHeader(r, bytes);
  EXPECT_FALSE(parseRecordHeader(bytes, back));
}
