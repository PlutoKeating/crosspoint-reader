#include "FrameStore.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef SIMULATOR
#include <Arduino.h>
#endif

using frame_slots::FRAME_BYTES;
using frame_slots::NO_SLOT;
using frame_slots::SLOT_BYTES;

namespace frame_store {
namespace {
constexpr const char* ROOT = "/.crosspoint/studio";
std::atomic<bool> preparingNow{false};
bool prepareTried = false;
constexpr size_t BLOCK = 512;

// Hashes `bytes` of `file` from `start`; the block lives on the heap only for
// the call (no static RAM for a path that runs at boot and per transfer).
bool sha256Of(HalFile& file, size_t start, size_t bytes, mbedtls_sha256_context& sha) {
  auto block = makeUniqueNoThrow<uint8_t[]>(BLOCK);
  if (!block || !file.seek(start)) return false;
  while (bytes) {
    const size_t count = std::min(bytes, BLOCK);
    if (file.read(block.get(), count) != static_cast<int>(count)) return false;
    mbedtls_sha256_update(&sha, block.get(), count);
    bytes -= count;
  }
  return true;
}
std::string hexOf(const uint8_t digest[32]) {
  char text[65];
  for (size_t i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", digest[i]);
  return text;
}
}  // namespace

std::string recordFor(const std::string& hash) { return std::string(ROOT) + "/" + hash + ".ssp"; }
std::string fileFor(const std::string& hash) { return std::string(ROOT) + "/" + hash + ".bin"; }

bool preparing() { return preparingNow.load(); }
bool areaReady() { return Storage.exists(AREA) && Storage.exists(INDEX); }
bool firmwareAreaReady() { return Storage.exists(FIRMWARE_AREA); }
void discardFirmware() {
  Storage.remove(FIRMWARE_TEMP);
  Storage.remove(FIRMWARE_META);
}

bool resetIndex() {
  auto bytes = makeUniqueNoThrow<uint8_t[]>(frame_slots::INDEX_BYTES);
  if (!bytes) return false;
  frame_slots::Index::serializeEmpty(bytes.get());
  HalFile file = Storage.open(INDEX, O_RDWR | O_CREAT | O_TRUNC);
  const bool ok = file && file.write(bytes.get(), frame_slots::INDEX_BYTES) == frame_slots::INDEX_BYTES;
  file.close();
  return ok;
}

bool prepare() {
  const bool area = Storage.exists(AREA), index = Storage.exists(INDEX), firmware = Storage.exists(FIRMWARE_AREA);
  if (area && index && firmware) return true;
  // One attempt per boot: on a full or fragmented card the cluster scan would
  // otherwise repeat (and show its notice) at every housekeeping round.
  if (prepareTried) return area && index;
  prepareTried = true;
  preparingNow.store(true);
  Storage.mkdir(ROOT, true);
  bool ok = area && index;
  if (!area) {
    const uint32_t startedMs = millis();
    // A new area holds no frames: whatever an old index says is void.
    ok = Storage.createContiguous(AREA, frame_slots::AREA_BYTES) && resetIndex();
    LOG_INF("STUDIO", "Frame area %s (%u bytes, %u ms)", ok ? "created" : "NOT created",
            (unsigned)frame_slots::AREA_BYTES, (unsigned)(millis() - startedMs));
    if (!ok) Storage.remove(AREA);
  } else if (!index) {
    ok = resetIndex();
  }
  if (!firmware) {
    const uint32_t startedMs = millis();
    const bool made = Storage.createContiguous(FIRMWARE_AREA, FIRMWARE_AREA_BYTES);
    LOG_INF("STUDIO", "Firmware area %s (%u bytes, %u ms)", made ? "created" : "NOT created",
            (unsigned)FIRMWARE_AREA_BYTES, (unsigned)(millis() - startedMs));
  }
  preparingNow.store(false);
  return ok;
}

bool readRecord(const std::string& path, frame_slots::Record& record, std::vector<uint16_t>& slots) {
  HalFile file;
  if (!Storage.exists(path.c_str()) || !Storage.openFileForRead("STUDIO", path, file)) return false;
  uint8_t header[frame_slots::RECORD_HEADER];
  if (file.read(header, sizeof(header)) != sizeof(header) || !frame_slots::parseRecordHeader(header, record)) return false;
  slots.assign(record.count, NO_SLOT);
  uint8_t pair[2];
  for (auto& slot : slots) {
    if (file.read(pair, 2) != 2) return false;
    slot = uint16_t(pair[0] | pair[1] << 8);
  }
  for (uint32_t i = 0; i < record.done; ++i)
    if (slots[i] >= frame_slots::SLOTS) return false;
  return file.size() >= record.headerOffset() + record.headerBytes;
}

bool locate(const std::string& hash, const size_t size, const size_t at, std::string& path, size_t& start) {
  frame_slots::Record record;
  std::vector<uint16_t> slots;
  const std::string recordPath = recordFor(hash);
  if (!Storage.exists(recordPath.c_str())) {
    path = fileFor(hash);
    start = at;
    return true;
  }
  if (!readRecord(recordPath, record, slots) || record.done != record.count || record.contentBytes() != size ||
      at < record.headerBytes)
    return false;
  const size_t frame = (at - record.headerBytes) / FRAME_BYTES;
  if (frame >= record.count) return false;
  path = AREA;
  start = size_t(slots[frame]) * SLOT_BYTES + (at - record.headerBytes) % FRAME_BYTES;
  return true;
}

bool headerOf(const std::string& hash, const size_t size, std::string& path, size_t& start) {
  frame_slots::Record record;
  std::vector<uint16_t> slots;
  const std::string recordPath = recordFor(hash);
  if (!Storage.exists(recordPath.c_str())) {
    path = fileFor(hash);
    start = 0;
    return true;
  }
  if (!readRecord(recordPath, record, slots) || record.contentBytes() != size || record.headerBytes == 0)
    return false;
  path = recordPath;
  start = record.headerOffset();
  return true;
}

bool verify(const std::string& hash, const size_t size) {
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool valid = false;
  const std::string recordPath = recordFor(hash);
  if (Storage.exists(recordPath.c_str())) {
    frame_slots::Record record;
    std::vector<uint16_t> slots;
    HalFile header, area;
    valid = readRecord(recordPath, record, slots) && record.done == record.count && record.contentBytes() == size &&
            Storage.openFileForRead("STUDIO", recordPath, header) &&
            sha256Of(header, record.headerOffset(), record.headerBytes, sha) &&
            Storage.openFileForRead("STUDIO", AREA, area);
    for (size_t i = 0; valid && i < slots.size(); ++i)
      valid = sha256Of(area, size_t(slots[i]) * SLOT_BYTES, FRAME_BYTES, sha);
  } else {
    HalFile file;
    valid = Storage.openFileForRead("STUDIO", fileFor(hash), file) && file.size() == size &&
            sha256Of(file, 0, size, sha);
  }
  uint8_t sum[32];
  mbedtls_sha256_finish(&sha, sum);
  mbedtls_sha256_free(&sha);
  return valid && hexOf(sum) == hash;
}

size_t Incoming::partialBytes() {
  frame_slots::Record record;
  std::vector<uint16_t> slots;
  if (!readRecord(RECORD_TEMP, record, slots)) return 0;
  const bool single = record.count == 1 && record.headerBytes == 0;
  if (!single && record.headerBytes == 0) return 0;  // the header was not complete
  return record.headerBytes + size_t(record.done) * FRAME_BYTES;
}

bool Incoming::begin(const size_t size, const std::vector<std::string>& kept, const bool resume,
                     const size_t resumeLimit, mbedtls_sha256_context& sha, size_t& offset) {
  close();
  offset = 0;
  size_ = size;
  const size_t frames = frame_slots::framesIn(size);
  if (!frames || frames > frame_slots::SLOTS || !areaReady()) return false;
  {
    auto bytes = makeUniqueNoThrow<uint8_t[]>(frame_slots::INDEX_BYTES);
    HalFile file;
    if (!bytes || !Storage.openFileForRead("STUDIO", INDEX, file)) return false;
    const bool read = file.read(bytes.get(), frame_slots::INDEX_BYTES) == int(frame_slots::INDEX_BYTES);
    file.close();
    // A damaged index only loses the cache: the records say which slots hold
    // kept frames, and those are pinned below.
    if (!read || !index_.parse(bytes.get(), frame_slots::INDEX_BYTES)) {
      LOG_ERR("STUDIO", "Frame index unreadable: starting it over");
      index_.reset();
      if (!resetIndex()) return false;
    }
  }
  pins_.reset();
  for (const auto& hash : kept) {
    frame_slots::Record record;
    std::vector<uint16_t> slots;
    if (hash.empty() || !readRecord(recordFor(hash), record, slots)) continue;
    for (uint32_t i = 0; i < record.done; ++i) pins_.set(slots[i]);
  }
  area_ = Storage.open(AREA, O_RDWR);
  indexFile_ = Storage.open(INDEX, O_RDWR);
  if (!area_ || !indexFile_ || area_.size() != frame_slots::AREA_BYTES) {
    LOG_ERR("STUDIO", "Frame area unusable (size %u)", area_ ? (unsigned)area_.size() : 0u);
    close();
    return false;
  }
  if (!area_.isContiguous()) LOG_ERR("STUDIO", "Frame area is fragmented: writes go through the FAT");

  bool resumed = false;
  if (resume && readRecord(RECORD_TEMP, record_, slots_) && record_.count == frames) {
    const bool single = size == FRAME_BYTES;
    const size_t header = record_.headerBytes;
    if ((single || header) && header + size_t(record_.count) * FRAME_BYTES == size) {
      // Resume at a frame boundary no later than the caller's limit.
      uint32_t done = record_.done;
      if (header + size_t(done) * FRAME_BYTES > resumeLimit)
        done = resumeLimit < header ? 0 : uint32_t((resumeLimit - header) / FRAME_BYTES);
      const bool keepHeader = single || resumeLimit >= header;
      if (keepHeader) {
        HalFile file;
        bool ok = Storage.openFileForRead("STUDIO", RECORD_TEMP, file) &&
                  sha256Of(file, record_.headerOffset(), header, sha);
        file.close();
        for (uint32_t i = 0; ok && i < done; ++i) {
          pins_.set(slots_[i]);
          ok = sha256Of(area_, size_t(slots_[i]) * SLOT_BYTES, FRAME_BYTES, sha);
        }
        if (ok) {
          record_.done = done;
          offset = header + size_t(done) * FRAME_BYTES;
          resumed = true;
        } else {
          mbedtls_sha256_starts(&sha, 0);
        }
      }
    }
  }
  if (!resumed) {
    Storage.remove(RECORD_TEMP);
    record_ = {};
    record_.count = static_cast<uint16_t>(frames);
    slots_.assign(frames, NO_SLOT);
    memset(prefix_, 0, sizeof(prefix_));
  }
  const size_t needed = frames - record_.done;
  if (index_.available(pins_) < needed) {
    LOG_INF("STUDIO", "Frame slots: %u free, %u needed: whole-file transfer", (unsigned)index_.available(pins_),
            (unsigned)needed);
    close();
    offset = 0;
    mbedtls_sha256_starts(&sha, 0);
    return false;
  }
  recordFile_ = Storage.open(RECORD_TEMP, O_RDWR | O_CREAT);
  if (!recordFile_) {
    close();
    return false;
  }
  if (!resumed) {
    // Header and an empty slot table (0xffff); the SSP1 header follows.
    uint8_t empty[64];
    memset(empty, 0xff, sizeof(empty));
    bool ok = writeRecordHeader() && recordFile_.seek(frame_slots::RECORD_HEADER);
    for (size_t left = frames * 2; ok && left;) {
      const size_t n = std::min(left, sizeof(empty));
      ok = recordFile_.write(empty, n) == n;
      left -= n;
    }
    if (!ok) {
      close();
      return false;
    }
  } else {
    writeRecordHeader();  // `done` may have moved back to the limit
  }
  return true;
}

bool Incoming::writeRecordHeader() {
  uint8_t header[frame_slots::RECORD_HEADER];
  frame_slots::encodeRecordHeader(record_, header);
  return recordFile_.seek(0) && recordFile_.write(header, sizeof(header)) == sizeof(header);
}

bool Incoming::writeTable(const uint16_t frame) {
  const uint8_t pair[2] = {uint8_t(slots_[frame] & 0xff), uint8_t(slots_[frame] >> 8)};
  return recordFile_.seek(record_.slotsOffset() + size_t(frame) * 2) && recordFile_.write(pair, 2) == 2;
}

bool Incoming::writeEntry(const uint16_t slot) {
  uint8_t entry[frame_slots::ENTRY_BYTES];
  index_.serializeEntry(slot, entry);
  return indexFile_.seek(frame_slots::Index::entryOffset(slot)) &&
         indexFile_.write(entry, sizeof(entry)) == sizeof(entry);
}

bool Incoming::write(size_t offset, const uint8_t* data, size_t length) {
  if (!recordFile_ || !area_) return false;
  const bool single = size_ == FRAME_BYTES;
  while (length) {
    if (!single && record_.headerBytes == 0) {
      // SSP1 header: to the record, behind the slot table. Its length is in
      // the first 8 bytes.
      size_t end = 8;
      if (offset >= 8) {
        end = 8 + (size_t(prefix_[4]) | size_t(prefix_[5]) << 8 | size_t(prefix_[6]) << 16 |
                   size_t(prefix_[7]) << 24);
        if (end > 8 + 32768 || end + size_t(record_.count) * FRAME_BYTES != size_) return false;
      }
      const size_t n = std::min(length, end - offset);
      for (size_t i = 0; i < n && offset + i < 8; ++i) prefix_[offset + i] = data[i];
      if (!recordFile_.seek(record_.headerOffset() + offset) || recordFile_.write(data, n) != n) return false;
      offset += n;
      data += n;
      length -= n;
      if (offset >= 8 && offset == 8 + (size_t(prefix_[4]) | size_t(prefix_[5]) << 8 |
                                        size_t(prefix_[6]) << 16 | size_t(prefix_[7]) << 24)) {
        record_.headerBytes = static_cast<uint32_t>(offset);
        if (record_.headerBytes + size_t(record_.count) * FRAME_BYTES != size_ || !writeRecordHeader()) return false;
      }
      continue;
    }
    const size_t into = offset - record_.headerBytes;
    const size_t frame = into / FRAME_BYTES, at = into % FRAME_BYTES;
    if (frame >= record_.count) return false;
    if (at == 0) {
      // A new frame: the least recently stored slot no kept program uses. It
      // stops holding its old frame on the card before it is overwritten.
      const uint16_t slot = index_.pick(pins_);
      if (slot == NO_SLOT) return false;
      if (index_.entry(slot).valid) {
        index_.release(slot);
        if (!writeEntry(slot)) return false;
      }
      pins_.set(slot);
      slots_[frame] = slot;
      if (!area_.seek(size_t(slot) * SLOT_BYTES)) return false;
      mbedtls_sha256_init(&frameSha_);
      mbedtls_sha256_starts(&frameSha_, 0);
      frameOpen_ = true;
    } else if (!frameOpen_) {
      return false;
    }
    const size_t n = std::min<size_t>(length, FRAME_BYTES - at);
    if (area_.write(data, n) != n) return false;
    mbedtls_sha256_update(&frameSha_, data, n);
    offset += n;
    data += n;
    length -= n;
    if (at + n == FRAME_BYTES) {
      frame_slots::Digest digest;
      mbedtls_sha256_finish(&frameSha_, digest.data());
      mbedtls_sha256_free(&frameSha_);
      frameOpen_ = false;
      index_.store(slots_[frame], digest);
      record_.done = static_cast<uint32_t>(frame + 1);
      // Order: frame data, its index entry, then the record that references it.
      area_.flush();
      if (!writeEntry(slots_[frame])) return false;
      indexFile_.flush();
      if (!writeTable(static_cast<uint16_t>(frame)) || !writeRecordHeader()) return false;
    }
  }
  return true;
}

bool Incoming::reference(const size_t offset, const uint16_t slot, mbedtls_sha256_context& sha) {
  if (!recordFile_ || !area_ || slot >= frame_slots::SLOTS) return false;
  const bool single = size_ == FRAME_BYTES;
  if (!single && record_.headerBytes == 0) return false;
  const size_t into = offset - record_.headerBytes;
  if (into % FRAME_BYTES || into / FRAME_BYTES >= record_.count || frameOpen_) return false;
  const size_t frame = into / FRAME_BYTES;
  // The bytes are hashed for the whole-transfer digest; nothing is copied.
  if (!sha256Of(area_, size_t(slot) * SLOT_BYTES, FRAME_BYTES, sha)) return false;
  pins_.set(slot);
  slots_[frame] = slot;
  record_.done = static_cast<uint32_t>(frame + 1);
  return writeTable(static_cast<uint16_t>(frame)) && writeRecordHeader();
}

bool Incoming::headerSource(std::string& path, size_t& start) {
  if (!recordFile_ || record_.headerBytes == 0) return false;
  recordFile_.flush();
  path = RECORD_TEMP;
  start = record_.headerOffset();
  return true;
}

void Incoming::close() {
  if (frameOpen_) {
    mbedtls_sha256_free(&frameSha_);
    frameOpen_ = false;
  }
  if (recordFile_) recordFile_.close();
  if (area_) area_.close();
  if (indexFile_) indexFile_.close();
}

}  // namespace frame_store
