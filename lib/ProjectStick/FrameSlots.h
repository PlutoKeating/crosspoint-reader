#pragma once
// Frame slots (2.7.10): a preallocated, contiguous area on the SD card holding
// frames by digest, so a program references its frames instead of embedding
// copies. Frames a new program shares with the ones on the device are not
// received or copied again, and a program commits without moving frames.
//
//   frames.bin  SLOTS x SLOT_BYTES, created once, never deleted; slot s holds
//               one 52,272-byte frame at s * SLOT_BYTES (sector aligned).
//   frames.idx  INDEX_HEADER + SLOTS x ENTRY_BYTES: per slot its state, the
//               SHA-256 of the frame it holds and an allocation stamp.
//   <hash>.ssp  a program record: RECORD_HEADER, the slot of each frame, then
//               the SSP1 header bytes (none for a single frame).
//
// Pure (no SD access), so host tests cover allocation, eviction and the file
// formats; StudioFrame does the card I/O.
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace frame_slots {

constexpr uint32_t FRAME_BYTES = 52272;
constexpr uint32_t SLOT_BYTES = 52736;  // FRAME_BYTES rounded up to 103 sectors
constexpr uint16_t SLOTS = 128;
constexpr uint32_t AREA_BYTES = SLOT_BYTES * SLOTS;  // 6,750,208
constexpr size_t INDEX_HEADER = 16, ENTRY_BYTES = 40;
constexpr size_t INDEX_BYTES = INDEX_HEADER + SLOTS * ENTRY_BYTES;  // 5,136
constexpr size_t RECORD_HEADER = 16;
constexpr uint16_t NO_SLOT = 0xffff;

using Digest = std::array<uint8_t, 32>;
using Pins = std::bitset<SLOTS>;

struct Entry {
  Digest digest{};
  uint32_t stamp = 0;  // allocation order; the oldest unpinned frame is evicted first
  bool valid = false;
};

class Index {
 public:
  // All slots free (a new area, or clear()).
  void reset();
  // The whole frames.idx; false for a file of another layout.
  bool parse(const uint8_t* data, size_t size);
  void serialize(uint8_t out[INDEX_BYTES]) const;
  // An index with every slot free, without an Index (5 KB) on the stack.
  static void serializeEmpty(uint8_t out[INDEX_BYTES]);
  // Bytes of entry `slot` and their offset in frames.idx.
  static size_t entryOffset(uint16_t slot) { return INDEX_HEADER + size_t(slot) * ENTRY_BYTES; }
  void serializeEntry(uint16_t slot, uint8_t out[ENTRY_BYTES]) const;

  // The slot holding `digest`, or NO_SLOT.
  uint16_t find(const Digest& digest) const;
  // A slot for a new frame: a free one, else the valid one with the oldest
  // stamp that is not pinned. NO_SLOT when every slot is pinned.
  uint16_t pick(const Pins& pinned) const;
  // Slots pick() could hand out.
  size_t available(const Pins& pinned) const;
  // A slot about to be overwritten: no longer holds any digest.
  void release(uint16_t slot);
  // `slot` now holds the frame with `digest`.
  void store(uint16_t slot, const Digest& digest);
  const Entry& entry(uint16_t slot) const { return entries_[slot]; }

 private:
  std::array<Entry, SLOTS> entries_{};  // inline: one allocation for the owner
  uint32_t nextStamp_ = 1;
};

// <hash>.ssp: "SSPS", u16 version 1, u16 frame count, u32 SSP1 header bytes,
// u32 frames done (a partial record; == count once complete), then u16 slot
// per frame, then the header bytes. Little endian.
struct Record {
  uint16_t count = 0;
  uint32_t headerBytes = 0, done = 0;
  // Total SSP1 bytes the record stands for.
  size_t contentBytes() const { return size_t(headerBytes) + size_t(count) * FRAME_BYTES; }
  size_t slotsOffset() const { return RECORD_HEADER; }
  size_t headerOffset() const { return RECORD_HEADER + size_t(count) * 2; }
};
void encodeRecordHeader(const Record& record, uint8_t out[RECORD_HEADER]);
bool parseRecordHeader(const uint8_t in[RECORD_HEADER], Record& record);

// Frames in a transfer of `size` SSP1 bytes (the header is smaller than one
// frame, so this is exact); 0 for an impossible size.
inline size_t framesIn(size_t size) { return size / FRAME_BYTES; }

}  // namespace frame_slots
