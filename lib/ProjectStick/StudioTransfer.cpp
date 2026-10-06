#include "StudioTransfer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace studio_v4 {
namespace {
constexpr const char* ERROR_NAMES[] = {
    "",
    "unauthorized_or_invalid_chunk",
    "offset_mismatch",
    "device_busy",
    "storage_or_cipher_error",
    "frame_validation_failed",
    "frame_mismatch",
    "too_many_frames",
    "transfer_timeout",
    "insufficient_storage",
    "authorization_failed",
    "insufficient_memory",
};
constexpr size_t ERROR_COUNT = sizeof(ERROR_NAMES) / sizeof(ERROR_NAMES[0]);

void putLe32(uint8_t* out, uint32_t value) {
  out[0] = value & 0xff;
  out[1] = (value >> 8) & 0xff;
  out[2] = (value >> 16) & 0xff;
  out[3] = (value >> 24) & 0xff;
}
int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}
}  // namespace

const char* errorName(Error error) {
  const auto index = static_cast<size_t>(error);
  return index < ERROR_COUNT ? ERROR_NAMES[index] : "";
}

Error errorFromName(const std::string& name) {
  for (size_t i = 1; i < ERROR_COUNT; ++i)
    if (name == ERROR_NAMES[i]) return static_cast<Error>(i);
  return Error::None;
}

std::string beginMessage(const std::string& deviceId, const std::string& nonce, uint32_t epoch,
                         const std::string& task, const std::string& hash, int64_t expires, size_t size,
                         size_t header, int64_t time) {
  return "studio4|" + deviceId + "|" + nonce + "|" + std::to_string(epoch) + "|" + task + "|" + hash + "|" +
         std::to_string(expires) + "|" + std::to_string(size) + "|" + std::to_string(header) + "|" +
         std::to_string(time);
}

void encodeProgress(uint8_t out[PROGRESS_BYTES], uint32_t received, State state, Error error, uint16_t needCount,
                    uint32_t seq) {
  memset(out, 0, PROGRESS_BYTES);
  putLe32(out, received);
  out[4] = static_cast<uint8_t>(state);
  out[5] = static_cast<uint8_t>(error);
  out[6] = needCount & 0xff;
  out[7] = needCount >> 8;
  putLe32(out + 12, seq);
}

std::string needHex(const std::vector<bool>& need) {
  const size_t bytes = (need.size() + 7) / 8;
  std::string out(std::max<size_t>(bytes, 1) * 2, '0');
  const char* alphabet = "0123456789abcdef";
  for (size_t byte = 0; byte < bytes; ++byte) {
    uint8_t value = 0;
    for (size_t bit = 0; bit < 8 && byte * 8 + bit < need.size(); ++bit)
      if (need[byte * 8 + bit]) value |= 1u << bit;
    out[byte * 2] = alphabet[value >> 4];
    out[byte * 2 + 1] = alphabet[value & 15];
  }
  return out;
}

bool parseNeedHex(const std::string& hex, size_t frames, std::vector<bool>& need) {
  const size_t bytes = std::max<size_t>((frames + 7) / 8, 1);
  if (hex.size() != bytes * 2) return false;
  need.assign(frames, false);
  for (size_t byte = 0; byte < bytes; ++byte) {
    const int hi = nibble(hex[byte * 2]), lo = nibble(hex[byte * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    const unsigned value = unsigned(hi << 4 | lo);
    for (size_t bit = 0; bit < 8; ++bit) {
      const size_t index = byte * 8 + bit;
      const bool set = value & (1u << bit);
      if (index < frames)
        need[index] = set;
      else if (set)
        return false;
    }
  }
  return true;
}

bool parseDigest(const char* hex, Digest& out) {
  if (!hex || strlen(hex) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = nibble(hex[i * 2]), lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

// ---------------------------------------------------------------------------

void Assembler::reset() {
  inflater_.reset();
  io_ = nullptr;
  phase_ = Phase::Idle;
  error_ = Error::None;
  header_ = size_ = headerSeen_ = consumed_ = next_ = 0;
  digests_.clear();
  need_.clear();
  have_.clear();
  recordHeaderSeen_ = recordBytes_ = recordSeen_ = 0;
  inRecord_ = false;
}

bool Assembler::fail(Error error) {
  if (phase_ != Phase::Failed) error_ = error;
  phase_ = Phase::Failed;
  return false;
}

size_t Assembler::needCount() const { return static_cast<size_t>(std::count(need_.begin(), need_.end(), true)); }

bool Assembler::begin(Io& io, size_t header, size_t size, const Digest* singleDigest, size_t framesDone,
                      bool headerDone) {
  reset();
  io_ = &io;
  header_ = header;
  size_ = size;
  if (header == 0) {
    // A single frame: its digest is the transfer hash; no SSP1 header.
    if (!singleDigest || size != FRAME_BYTES || framesDone > 1) return fail(Error::FrameValidation);
    digests_.assign(1, *singleDigest);
    have_.assign(1, false);
    Error error = Error::None;
    std::vector<Digest> resolved;
    // The Io reports whether the frame is held locally (frames must be empty
    // to mean "single frame"; it fills `have`).
    if (!io.resolve(resolved, have_, error)) return fail(error == Error::None ? Error::FrameValidation : error);
    if (have_.size() != 1) return fail(Error::FrameValidation);
    next_ = framesDone;
    return startFrames();
  }
  if (header < 8 + 2 || header > 8 + 32768 || size < header + FRAME_BYTES) return fail(Error::FrameValidation);
  if (framesDone > 0 || headerDone) {
    headerSeen_ = header;
    next_ = framesDone;
    Error error = Error::None;
    if (!io.resolve(digests_, have_, error)) return fail(error == Error::None ? Error::FrameValidation : error);
    if (digests_.empty() || have_.size() != digests_.size() || header + digests_.size() * FRAME_BYTES != size ||
        next_ > digests_.size())
      return fail(Error::FrameValidation);
    return startFrames();
  }
  phase_ = Phase::Header;
  return true;
}

bool Assembler::startFrames() {
  need_.assign(digests_.size(), false);
  for (size_t i = next_; i < digests_.size(); ++i) need_[i] = !have_[i];
  phase_ = Phase::Frames;
  return advance();
}

bool Assembler::advance() {
  while (next_ < digests_.size() && !need_[next_]) {
    if (!io_->copyFrame(next_)) return fail(Error::StorageFailure);
    ++next_;
  }
  if (next_ == digests_.size()) phase_ = Phase::Complete;
  return true;
}

bool Assembler::feed(const uint8_t* data, size_t size) {
  if (phase_ == Phase::Failed) return false;
  while (size) {
    if (phase_ == Phase::Header) {
      const size_t take = std::min(size, header_ - headerSeen_);
      if (!io_->write(data, take)) return fail(Error::StorageFailure);
      headerSeen_ += take;
      consumed_ += take;
      data += take;
      size -= take;
      if (headerSeen_ == header_) {
        Error error = Error::None;
        if (!io_->resolve(digests_, have_, error)) return fail(error == Error::None ? Error::FrameValidation : error);
        if (digests_.size() > MAX_FRAMES) return fail(Error::TooManyFrames);
        if (digests_.empty() || have_.size() != digests_.size() || header_ + digests_.size() * FRAME_BYTES != size_)
          return fail(Error::FrameValidation);
        if (!startFrames()) return false;
      }
      continue;
    }
    if (phase_ != Phase::Frames) return fail(Error::InvalidChunk);  // bytes after the last record
    if (!inRecord_) {
      const size_t take = std::min(size, RECORD_HEADER_BYTES - recordHeaderSeen_);
      memcpy(recordHeader_ + recordHeaderSeen_, data, take);
      recordHeaderSeen_ += take;
      consumed_ += take;
      data += take;
      size -= take;
      if (recordHeaderSeen_ < RECORD_HEADER_BYTES) continue;
      const size_t index = size_t(recordHeader_[0]) | size_t(recordHeader_[1]) << 8;
      recordBytes_ = size_t(recordHeader_[2]) | size_t(recordHeader_[3]) << 8 | size_t(recordHeader_[4]) << 16 |
                     size_t(recordHeader_[5]) << 24;
      // Records arrive for the needed frames, in order: the next one is the
      // frame advance() stopped at.
      if (index != next_ || next_ >= digests_.size() || !need_[next_] || recordBytes_ == 0 ||
          recordBytes_ > MAX_RECORD_BYTES)
        return fail(Error::FrameMismatch);
      if (!io_->stageBegin()) return fail(Error::StorageFailure);
      recordSeen_ = 0;
      inRecord_ = true;
      continue;
    }
    const size_t take = std::min(size, recordBytes_ - recordSeen_);
    if (!io_->stageWrite(data, take)) return fail(Error::StorageFailure);
    recordSeen_ += take;
    consumed_ += take;
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
  return inflateRecord();
}

bool Assembler::inflateRecord() {
  // The inflater (uzlib state + 1 KB dictionary, ~2.9 KB) lives on the heap
  // for the frames phase only; the host has switched Wi-Fi off for the linked
  // phone by now, so this is the easy allocation of the transfer.
  if (!inflater_) inflater_.reset(new (std::nothrow) FrameInflater());
  if (!inflater_) return fail(Error::InsufficientMemory);
  const Error error = inflater_->inflate(*io_, digests_[next_]);
  if (error != Error::None) return fail(error);
  ++next_;
  return advance();
}

// ---------------------------------------------------------------------------

int FrameInflater::readByte(uzlib_uncomp* decomp) {
  auto* source = reinterpret_cast<Source*>(decomp);
  const int count = source->io->stageRead(source->buffer, sizeof(source->buffer));
  if (count <= 0) {
    if (count < 0) source->failed = true;
    return -1;
  }
  decomp->source = source->buffer + 1;
  decomp->source_limit = source->buffer + count;
  return source->buffer[0];
}

Error FrameInflater::inflate(Io& io, const Digest& expected) {
  static_assert(offsetof(Source, decomp) == 0, "uzlib callback casts the decompressor back to Source");
  memset(&source_, 0, sizeof(source_));
  source_.io = &io;
  uzlib_uncompress_init(&source_.decomp, dict_, sizeof(dict_));
  source_.decomp.source = nullptr;
  source_.decomp.source_limit = nullptr;
  source_.decomp.source_read_cb = &FrameInflater::readByte;
  ble_crypto::Sha256 sha;
  size_t produced = 0;
  for (;;) {
    source_.decomp.dest_start = out_;
    source_.decomp.dest = out_;
    source_.decomp.dest_limit = out_ + std::min(sizeof(out_), FRAME_BYTES + 1 - produced);
    const int result = uzlib_uncompress(&source_.decomp);
    const size_t count = static_cast<size_t>(source_.decomp.dest - out_);
    if (source_.failed) return Error::StorageFailure;
    if (result < 0) return Error::FrameMismatch;
    produced += count;
    if (produced > FRAME_BYTES) return Error::FrameMismatch;
    if (count) {
      sha.update(out_, count);
      if (!io.write(out_, count)) return Error::StorageFailure;
    }
    if (result == TINF_DONE) break;
    if (count == 0 && source_.decomp.eof) return Error::FrameMismatch;
  }
  if (produced != FRAME_BYTES) return Error::FrameMismatch;
  uint8_t digest[32];
  sha.finish(digest);
  return memcmp(digest, expected.data(), 32) == 0 ? Error::None : Error::FrameMismatch;
}

}  // namespace studio_v4
