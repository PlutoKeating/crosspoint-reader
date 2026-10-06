#include "FirmwareTransfer.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace firmware_v4 {

std::string beginMessage(const std::string& deviceId, const std::string& nonce, const uint32_t epoch,
                         const std::string& version, const std::string& sha256, const size_t size,
                         const int64_t time) {
  return "fw4|" + deviceId + "|" + nonce + "|" + std::to_string(epoch) + "|" + version + "|" + sha256 + "|" +
         std::to_string(size) + "|" + std::to_string(time);
}

void Assembler::reset() {
  inflater_.reset();
  sha_.reset();
  io_ = nullptr;
  phase_ = Phase::Idle;
  error_ = Error::None;
  size_ = rawDone_ = streamOffset_ = 0;
  recordHeaderSeen_ = recordBytes_ = recordSeen_ = 0;
  inRecord_ = false;
}

bool Assembler::fail(const Error error) {
  if (phase_ != Phase::Failed) error_ = error;
  phase_ = Phase::Failed;
  return false;
}

bool Assembler::begin(Io& io, const size_t size, const studio_v4::Digest& sha256, const size_t rawDone,
                      const size_t streamOffset) {
  reset();
  io_ = &io;
  // The install bounds (MIN/MAX_IMAGE_BYTES) are the op's guard; the stream
  // itself only needs whole blocks.
  if (size == 0 || size > MAX_IMAGE_BYTES || rawDone % BLOCK_BYTES != 0 || rawDone >= size ||
      (rawDone == 0) != (streamOffset == 0))
    return fail(Error::FrameValidation);
  size_ = size;
  rawDone_ = rawDone;
  streamOffset_ = streamOffset;
  expected_ = sha256;
  phase_ = Phase::Blocks;
  return true;
}

bool Assembler::feed(const uint8_t* data, size_t size) {
  if (phase_ == Phase::Failed) return false;
  while (size) {
    if (phase_ != Phase::Blocks) return fail(Error::InvalidChunk);  // bytes after the last block
    if (!inRecord_) {
      const size_t take = std::min(size, RECORD_HEADER_BYTES - recordHeaderSeen_);
      memcpy(recordHeader_ + recordHeaderSeen_, data, take);
      recordHeaderSeen_ += take;
      data += take;
      size -= take;
      if (recordHeaderSeen_ < RECORD_HEADER_BYTES) continue;
      const auto le32 = [](const uint8_t* p) {
        return size_t(p[0]) | size_t(p[1]) << 8 | size_t(p[2]) << 16 | size_t(p[3]) << 24;
      };
      const size_t rawOffset = le32(recordHeader_);
      recordBytes_ = le32(recordHeader_ + 4);
      // Blocks arrive in order: the next one starts where the output ends.
      if (rawOffset != rawDone_ || recordBytes_ == 0 || recordBytes_ > MAX_RECORD_BYTES)
        return fail(Error::FrameMismatch);
      if (!io_->stageBegin()) return fail(Error::StorageFailure);
      recordSeen_ = 0;
      inRecord_ = true;
      continue;
    }
    const size_t take = std::min(size, recordBytes_ - recordSeen_);
    if (!io_->stageWrite(data, take)) return fail(Error::StorageFailure);
    recordSeen_ += take;
    data += take;
    size -= take;
    if (recordSeen_ == recordBytes_ && !finishRecord()) return false;
  }
  return true;
}

bool Assembler::finishRecord() {
  inRecord_ = false;
  recordHeaderSeen_ = 0;
  if (!io_->stageRewind()) return fail(Error::StorageFailure);
  if (!inflater_) inflater_.reset(new (std::nothrow) studio_v4::FrameInflater());
  if (!inflater_) return fail(Error::InsufficientMemory);
  const size_t expected = blockBytes(size_, rawDone_);
  const Error error = inflater_->inflate(*io_, expected, nullptr, &sha_);
  if (error != Error::None) return fail(error);
  rawDone_ += expected;
  streamOffset_ += RECORD_HEADER_BYTES + recordBytes_;
  if (rawDone_ == size_) {
    uint8_t digest[32];
    sha_.finish(digest);
    if (memcmp(digest, expected_.data(), 32) != 0) return fail(Error::ChecksumMismatch);
    phase_ = Phase::Complete;
  }
  if (!io_->checkpoint(rawDone_, streamOffset_)) return fail(Error::StorageFailure);
  return true;
}

}  // namespace firmware_v4
