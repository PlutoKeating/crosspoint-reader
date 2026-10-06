#pragma once
// BLE content transfer protocol 4 (Project.StockStick
// docs/product/BLE-TRANSFER-V4.md): the parts shared by the firmware and host
// tests. Proof message, the 16-byte PROGRESS value, the `need` bitmap and the
// Assembler that turns the decrypted stream (SSP1 header, then deflate records
// for the frames the device lacks) back into the full SSP1 file.
#include <uzlib.h>

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "BleCrypto.h"

namespace studio_v4 {

constexpr size_t FRAME_BYTES = 52272;
constexpr size_t MAX_FRAMES = 512;
constexpr size_t RECORD_HEADER_BYTES = 6;
// Raw deflate never expands a 52,272-byte frame beyond stored blocks (5 bytes
// per 65,535): anything larger is a protocol error.
constexpr size_t MAX_RECORD_BYTES = FRAME_BYTES + 1024;
// Window of the phone's compressor (windowBits 10) = uzlib dictionary.
constexpr size_t DICT_BYTES = 1024;
constexpr size_t PROGRESS_BYTES = 16;

enum class State : uint8_t {
  Idle = 0,
  ReceivingHeader = 1,
  ReceivingFrames = 2,
  Committing = 3,
  Displayed = 4,
  Scheduled = 5,
  Failed = 6,
  Paused = 7,
};

enum class Error : uint8_t {
  None = 0,
  InvalidChunk = 1,
  OffsetMismatch = 2,
  DeviceBusy = 3,
  StorageFailure = 4,  // not "Storage": HalStorage.h defines that macro
  FrameValidation = 5,
  FrameMismatch = 6,
  TooManyFrames = 7,
  TransferTimeout = 8,
  InsufficientStorage = 9,
  Authorization = 10,
  InsufficientMemory = 11,  // no heap for the transfer buffers (begin4 / first record)
  // Firmware over BLE (op fw4, 2.7.4).
  ChecksumMismatch = 12,  // the received image's SHA-256 differs from the announced one
  LowBattery = 13,        // below 30 % and not charging
  TrialActive = 14,       // a new image is still on its trial boot
};
// STATUS `error` string for a code ("" for None) and the reverse (None for an
// unknown name, which the PROGRESS value then reports as 0).
const char* errorName(Error error);
Error errorFromName(const std::string& name);

// mac(K, "studio4|device_id|N|epoch|task|hash|expires|size|header|time").
std::string beginMessage(const std::string& deviceId, const std::string& nonce, uint32_t epoch,
                         const std::string& task, const std::string& hash, int64_t expires, size_t size,
                         size_t header, int64_t time);

// PROGRESS: u32 received, u8 state, u8 error, u16 need_count, u32 0, u32 seq.
void encodeProgress(uint8_t out[PROGRESS_BYTES], uint32_t received, State state, Error error, uint16_t needCount,
                    uint32_t seq);

// One bit per frame, frame i = bit (i % 8) of byte i / 8, lowercase hex.
std::string needHex(const std::vector<bool>& need);
bool parseNeedHex(const std::string& hex, size_t frames, std::vector<bool>& need);

using Digest = std::array<uint8_t, 32>;
bool parseDigest(const char* hex, Digest& out);

// Sequential output plus a staging area for one compressed record: what the
// record inflater needs. Shared by content transfers (Io below) and firmware
// transfers (FirmwareTransfer.h). The device writes to the SD card, host tests
// to memory. Every call reports success; a false return fails the transfer
// with Error::StorageFailure.
class RecordIo {
 public:
  virtual ~RecordIo() = default;
  // Sequential output of the reconstructed file.
  virtual bool write(const uint8_t* data, size_t size) = 0;
  // Staging area for one compressed record, read back to inflate it.
  virtual bool stageBegin() = 0;
  virtual bool stageWrite(const uint8_t* data, size_t size) = 0;
  virtual bool stageRewind() = 0;
  // Returns bytes read (0 at the end), or -1 on an error.
  virtual int stageRead(uint8_t* data, size_t size) = 0;
};

// Content transfers: the SSP1 file (header, then frames in order).
class Io : public RecordIo {
 public:
  // The SSP1 header has been written: report the frame digests and which
  // frames the device already holds (frames before `firstFrame` are already in
  // the output on a resume and are ignored). Return false for an invalid header.
  virtual bool resolve(std::vector<Digest>& frames, std::vector<bool>& have, Error& error) = 0;
  // Appends locally held frame `index` (have[index] was true) to the output.
  virtual bool copyFrame(size_t index) = 0;
};

// Consumes the decrypted stream in order. The caller checks stream offsets and
// decrypts; the Assembler validates the content and drives the Io.
class Assembler {
 public:
  enum class Phase : uint8_t { Idle, Header, Frames, Complete, Failed };

  // `header`: SSP1 header bytes (8 + JSON), 0 for a single frame whose digest
  // is `singleDigest` (then `size` must be FRAME_BYTES). `framesDone`: frames
  // already in the output (resume; the header too when framesDone > 0 or
  // `headerDone`). Returns false on invalid parameters.
  bool begin(Io& io, size_t header, size_t size, const Digest* singleDigest, size_t framesDone, bool headerDone);
  // Plaintext stream bytes. Returns false (and Phase::Failed) on an error.
  bool feed(const uint8_t* data, size_t size);
  void reset();

  Phase phase() const { return phase_; }
  Error error() const { return error_; }
  // Valid from Phase::Frames on.
  const std::vector<bool>& need() const { return need_; }
  size_t needCount() const;
  size_t frames() const { return digests_.size(); }
  // Frames in the output so far (copied or inflated).
  size_t framesWritten() const { return next_; }
  // Plaintext bytes the Assembler consumed since begin().
  size_t consumed() const { return consumed_; }

 private:
  bool fail(Error error);
  bool startFrames();
  bool advance();  // copies local frames until the next needed one (or the end)
  bool finishRecord();
  bool inflateRecord();

  // Allocated for the frames phase of a transfer and released with reset():
  // ~3 KB that only exist while a phone sends (2.7.2).
  std::unique_ptr<class FrameInflater> inflater_;
  Io* io_ = nullptr;
  Phase phase_ = Phase::Idle;
  Error error_ = Error::None;
  size_t header_ = 0, size_ = 0, headerSeen_ = 0, consumed_ = 0;
  size_t next_ = 0;  // index of the next frame to write
  std::vector<Digest> digests_;
  std::vector<bool> need_, have_;
  // Current record.
  uint8_t recordHeader_[RECORD_HEADER_BYTES]{};
  size_t recordHeaderSeen_ = 0, recordBytes_ = 0, recordSeen_ = 0;
  bool inRecord_ = false;
};

// Inflates one raw-deflate record from `io`'s staging area into `io.write`,
// checking that it produces exactly `expectedBytes`. With `expected` the
// output's digest must match it (frames); `running` additionally receives the
// output (the whole-image hash of a firmware transfer). Fixed-size state
// (uzlib, a 1 KB dictionary and small buffers) lives in this object; it is
// allocated for the data phase of a transfer only.
class FrameInflater {
 public:
  Error inflate(RecordIo& io, size_t expectedBytes, const Digest* expected, ble_crypto::Sha256* running = nullptr);

 private:
  struct Source {
    uzlib_uncomp decomp;  // must stay first: the uzlib callback casts back
    RecordIo* io;
    uint8_t buffer[256];
    bool failed;
  } source_{};
  uint8_t dict_[DICT_BYTES]{};
  uint8_t out_[512]{};
  static int readByte(uzlib_uncomp* decomp);
};

}  // namespace studio_v4
