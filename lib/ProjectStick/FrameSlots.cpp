#include "FrameSlots.h"

#include <cstring>

namespace frame_slots {
namespace {
constexpr uint8_t INDEX_MAGIC[4] = {'F', 'S', 'I', '1'};
constexpr uint8_t RECORD_MAGIC[4] = {'S', 'S', 'P', 'S'};
void put16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = v >> 8;
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xff;
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}  // namespace

void Index::reset() {
  for (auto& e : entries_) e = Entry{};
  nextStamp_ = 1;
}

bool Index::parse(const uint8_t* data, const size_t size) {
  if (size != INDEX_BYTES || memcmp(data, INDEX_MAGIC, 4) != 0 || get16(data + 4) != SLOTS ||
      get16(data + 6) != ENTRY_BYTES)
    return false;
  nextStamp_ = 1;
  for (uint16_t s = 0; s < SLOTS; ++s) {
    const uint8_t* p = data + entryOffset(s);
    Entry& e = entries_[s];
    memcpy(e.digest.data(), p, 32);
    e.stamp = get32(p + 32);
    e.valid = p[36] == 1;
    if (!e.valid) e = Entry{};
    if (e.valid && e.stamp >= nextStamp_) nextStamp_ = e.stamp + 1;
  }
  return true;
}

void Index::serializeEmpty(uint8_t out[INDEX_BYTES]) {
  memset(out, 0, INDEX_BYTES);
  memcpy(out, INDEX_MAGIC, 4);
  put16(out + 4, SLOTS);
  put16(out + 6, ENTRY_BYTES);
}

void Index::serialize(uint8_t out[INDEX_BYTES]) const {
  serializeEmpty(out);
  for (uint16_t s = 0; s < SLOTS; ++s) serializeEntry(s, out + entryOffset(s));
}

void Index::serializeEntry(const uint16_t slot, uint8_t out[ENTRY_BYTES]) const {
  const Entry& e = entries_[slot];
  memset(out, 0, ENTRY_BYTES);
  if (!e.valid) return;
  memcpy(out, e.digest.data(), 32);
  put32(out + 32, e.stamp);
  out[36] = 1;
}

uint16_t Index::find(const Digest& digest) const {
  for (uint16_t s = 0; s < SLOTS; ++s)
    if (entries_[s].valid && entries_[s].digest == digest) return s;
  return NO_SLOT;
}

uint16_t Index::pick(const Pins& pinned) const {
  uint16_t oldest = NO_SLOT;
  for (uint16_t s = 0; s < SLOTS; ++s) {
    if (pinned.test(s)) continue;
    if (!entries_[s].valid) return s;
    if (oldest == NO_SLOT || entries_[s].stamp < entries_[oldest].stamp) oldest = s;
  }
  return oldest;
}

size_t Index::available(const Pins& pinned) const { return SLOTS - pinned.count(); }

void Index::release(const uint16_t slot) { entries_[slot] = Entry{}; }

void Index::store(const uint16_t slot, const Digest& digest) {
  entries_[slot] = {digest, nextStamp_++, true};
}

void encodeRecordHeader(const Record& record, uint8_t out[RECORD_HEADER]) {
  memset(out, 0, RECORD_HEADER);
  memcpy(out, RECORD_MAGIC, 4);
  put16(out + 4, 1);
  put16(out + 6, record.count);
  put32(out + 8, record.headerBytes);
  put32(out + 12, record.done);
}

bool parseRecordHeader(const uint8_t in[RECORD_HEADER], Record& record) {
  if (memcmp(in, RECORD_MAGIC, 4) != 0 || get16(in + 4) != 1) return false;
  record.count = get16(in + 6);
  record.headerBytes = get32(in + 8);
  record.done = get32(in + 12);
  return record.count > 0 && record.done <= record.count && record.headerBytes <= 8 + 32768;
}

}  // namespace frame_slots
