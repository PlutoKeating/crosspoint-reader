#pragma once
// Card side of the frame slots (lib/ProjectStick/FrameSlots.h, 2.7.10): the
// preallocated areas, program records (<hash>.ssp) and the writer a content
// transfer uses while it streams into slots. Content still stored as a whole
// SSP1 file (<hash>.bin: 2.7.9 and earlier, or a transfer that did not fit the
// slots) is read through the same calls.
//
// Not thread-safe by itself: StudioFrame serialises the writer; the readers
// only open files.
#include <FrameSlots.h>
#include <HalStorage.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace frame_store {

constexpr const char* AREA = "/.crosspoint/studio/frames.bin";
constexpr const char* INDEX = "/.crosspoint/studio/frames.idx";
constexpr const char* RECORD_TEMP = "/.crosspoint/studio/incoming.ssp";
// Firmware received over BLE or downloaded over Wi-Fi (6.25 MB, the OTA slot).
constexpr const char* FIRMWARE_AREA = "/.crosspoint/studio/firmware.area";
constexpr uint32_t FIRMWARE_AREA_BYTES = 6553600;

std::string recordFor(const std::string& hash);
std::string fileFor(const std::string& hash);  // the whole-file form

// Creates the areas that do not exist yet (once per card; they are never
// deleted). Seconds on a large card: the background worker calls it at idle
// while preparing() shows 「正在准备存储」. Returns whether the frame area is usable.
bool prepare();
bool preparing();
bool areaReady();
bool firmwareAreaReady();
// Drops a received or downloaded image (firmware.tmp and firmware.meta); the
// area stays, its bytes are simply no longer named by a meta.
void discardFirmware();
constexpr const char* FIRMWARE_TEMP = "/.crosspoint/studio/firmware.tmp";
constexpr const char* FIRMWARE_META = "/.crosspoint/studio/firmware.meta";
// Forgets every frame (a new owner): the index is rewritten empty.
bool resetIndex();

// Where `count` bytes of content `hash` (`size` SSP1 bytes) starting at SSP1
// offset `at` can be read: a frame of a record, or a range of a whole file.
bool locate(const std::string& hash, size_t size, size_t at, std::string& path, size_t& start);
// The SSP1 header (8 + JSON bytes) of a program.
bool headerOf(const std::string& hash, size_t size, std::string& path, size_t& start);
// SHA-256 of the content equals `hash` (boot check of the kept content).
bool verify(const std::string& hash, size_t size);
// A record's header and slot table; false for a missing or damaged record.
bool readRecord(const std::string& path, frame_slots::Record& record, std::vector<uint16_t>& slots);

// The slot-mode half of a content transfer: the SSP1 header goes to
// incoming.ssp, each frame to a slot picked from the index (never one a kept
// program uses), and the record is renamed to <hash>.ssp at commit. Frames the
// device already holds are referenced, not copied.
class Incoming {
 public:
  // Opens the transfer, resuming the record on the card when `resume`, up to
  // `resumeLimit` bytes; hashes the resumed bytes into `sha` and returns their
  // count in `offset`. False: no area, not enough unpinned slots, or a card
  // error — the caller falls back to the whole-file path.
  bool begin(size_t size, const std::vector<std::string>& kept, bool resume, size_t resumeLimit,
             mbedtls_sha256_context& sha, size_t& offset);
  // SSP1 bytes at `offset`, in order.
  bool write(size_t offset, const uint8_t* data, size_t length);
  // The slot holding a frame with this digest, or NO_SLOT.
  uint16_t find(const frame_slots::Digest& digest) const { return index_.find(digest); }
  // The next frame (at `offset`, a frame boundary) is the one in `slot`: hashed
  // into `sha`, referenced, not copied.
  bool reference(size_t offset, uint16_t slot, mbedtls_sha256_context& sha);
  // Where the received SSP1 header is (to read the frame digests back).
  bool headerSource(std::string& path, size_t& start);
  void close();
  // Bytes a record partial on the card holds (frame boundary), for resumeOffset().
  static size_t partialBytes();

 private:
  bool writeEntry(uint16_t slot);
  bool writeTable(uint16_t frame);
  bool writeRecordHeader();
  frame_slots::Index index_;
  frame_slots::Pins pins_;
  frame_slots::Record record_;
  std::vector<uint16_t> slots_;
  HalFile recordFile_, area_, indexFile_;
  mbedtls_sha256_context frameSha_{};
  bool frameOpen_ = false;
  size_t size_ = 0;
  uint8_t prefix_[8]{};
};

}  // namespace frame_store
