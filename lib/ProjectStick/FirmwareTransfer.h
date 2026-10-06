#pragma once
// Firmware over BLE (op fw4, firmware 2.7.4; Project.StockStick
// docs/product/BLE-TRANSFER-V4.md 「固件经蓝牙传输」): the parts shared by the
// firmware and host tests. The phone downloads the image, cuts it into
// 32,768-byte blocks, raw-deflates each one (1 KB window) and streams records
// `uint32LE raw_offset | uint32LE comp_len | deflate`, encrypted like a
// content transfer. The Assembler inflates each block straight into the
// output (firmware.tmp on the SD card) while hashing it; nothing larger than
// one compressed block is ever staged, and that on the card.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "BleCrypto.h"
#include "StudioTransfer.h"

namespace firmware_v4 {

constexpr size_t BLOCK_BYTES = 32768;
constexpr size_t RECORD_HEADER_BYTES = 8;
// Raw deflate of a 32 KB block never needs more than stored blocks (5 bytes
// per 65,535); anything larger is a protocol error.
constexpr size_t MAX_RECORD_BYTES = BLOCK_BYTES + 1024;
// Image size bounds (one OTA slot is 0x640000 bytes).
constexpr size_t MIN_IMAGE_BYTES = 100000;
constexpr size_t MAX_IMAGE_BYTES = 6553600;

// mac(K, "fw4|device_id|N|epoch|version|sha256|size|time").
std::string beginMessage(const std::string& deviceId, const std::string& nonce, uint32_t epoch,
                         const std::string& version, const std::string& sha256, size_t size, int64_t time);

// Length of the record for the block that starts at `rawOffset` is
// min(BLOCK_BYTES, size - rawOffset) once inflated.
inline size_t blockBytes(size_t size, size_t rawOffset) {
  return rawOffset >= size ? 0 : (size - rawOffset < BLOCK_BYTES ? size - rawOffset : BLOCK_BYTES);
}

// Output and staging (studio_v4::RecordIo) plus a resume checkpoint: called
// after every completed block, when the output holds `rawDone` image bytes
// and the stream has been consumed up to `streamOffset`.
class Io : public studio_v4::RecordIo {
 public:
  virtual bool checkpoint(size_t rawDone, size_t streamOffset) = 0;
};

class Assembler {
 public:
  enum class Phase : uint8_t { Idle, Blocks, Complete, Failed };
  using Error = studio_v4::Error;

  // Starts (or resumes) an image of `size` bytes whose SHA-256 is `sha256`.
  // On a resume the output already holds `rawDone` bytes (a block boundary)
  // and the stream continues at `streamOffset`; feed those bytes to
  // hashPrefix() before the first feed(). Returns false on invalid parameters.
  bool begin(Io& io, size_t size, const studio_v4::Digest& sha256, size_t rawDone, size_t streamOffset);
  void hashPrefix(const uint8_t* data, size_t size) { sha_.update(data, size); }
  // Plaintext stream bytes. Returns false (and Phase::Failed) on an error.
  // Reaching `size` image bytes checks the SHA-256 (Phase::Complete or
  // Error::ChecksumMismatch).
  bool feed(const uint8_t* data, size_t size);
  void reset();

  Phase phase() const { return phase_; }
  Error error() const { return error_; }
  size_t rawDone() const { return rawDone_; }
  size_t streamOffset() const { return streamOffset_; }
  size_t size() const { return size_; }

 private:
  bool fail(Error error);
  bool finishRecord();

  std::unique_ptr<studio_v4::FrameInflater> inflater_;  // first record to reset()
  ble_crypto::Sha256 sha_;
  studio_v4::Digest expected_{};
  Io* io_ = nullptr;
  Phase phase_ = Phase::Idle;
  Error error_ = Error::None;
  size_t size_ = 0, rawDone_ = 0, streamOffset_ = 0;
  uint8_t recordHeader_[RECORD_HEADER_BYTES]{};
  size_t recordHeaderSeen_ = 0, recordBytes_ = 0, recordSeen_ = 0;
  bool inRecord_ = false;
};

}  // namespace firmware_v4
