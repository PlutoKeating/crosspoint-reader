#include "FirmwareReceiver.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

using studio_v4::Error;

namespace {
constexpr const char* FIRMWARE_TEMP = "/.crosspoint/studio/firmware.tmp";
// Shared with the on-device Wi-Fi download, which writes "<sha256> <size>";
// a BLE transfer writes "<sha256> <size> <raw done> <stream offset>" after
// every block. Either path restarts the other's partial.
constexpr const char* FIRMWARE_META = "/.crosspoint/studio/firmware.meta";
constexpr const char* BLOCK_STAGE = "/.crosspoint/studio/firmware.z";
// Rehash buffer for a resumed partial; only the pump task runs the receiver.
uint8_t block[512];

bool readMeta(std::string& sha, size_t& size, size_t& rawDone, size_t& streamOffset) {
  HalFile file;
  if (!Storage.openFileForRead("FWBLE", FIRMWARE_META, file)) return false;
  char text[160] = {};
  const int read = file.read(text, sizeof(text) - 1);
  file.close();
  if (read <= 0) return false;
  char hash[65] = {};
  unsigned long s = 0, raw = 0, offset = 0;
  if (sscanf(text, "%64s %lu %lu %lu", hash, &s, &raw, &offset) != 4) return false;
  sha = hash;
  size = s;
  rawDone = raw;
  streamOffset = offset;
  return true;
}
}  // namespace

FirmwareReceiver& FirmwareReceiver::instance() {
  static FirmwareReceiver receiver;
  return receiver;
}

const char* FirmwareReceiver::path() { return FIRMWARE_TEMP; }

void FirmwareReceiver::closeFiles() {
  if (output) output.close();
  if (stage) stage.close();
  stageBytes = stageReadAt = 0;
}

void FirmwareReceiver::abort() {
  // The partial and its checkpoint stay on the card for a resume.
  closeFiles();
  running = false;
  assembler.reset();
}

bool FirmwareReceiver::start(const std::string& sha256, const size_t imageSize) {
  abort();
  failure = Error::None;
  sha = sha256;
  size = imageSize;
  studio_v4::Digest digest{};
  if (!studio_v4::parseDigest(sha.c_str(), digest)) {
    failure = Error::FrameValidation;
    return false;
  }
  Storage.mkdir("/.crosspoint/studio");
  // Resume a partial of the same image at its last checkpoint (a block
  // boundary); anything else starts over.
  std::string metaSha;
  size_t metaSize = 0, rawDone = 0, streamOffset = 0;
  bool resume = readMeta(metaSha, metaSize, rawDone, streamOffset) && metaSha == sha && metaSize == size &&
                rawDone > 0 && rawDone < size && rawDone % firmware_v4::BLOCK_BYTES == 0;
  if (resume) {
    HalFile existing;
    resume = Storage.openFileForRead("FWBLE", FIRMWARE_TEMP, existing) && existing.size() >= rawDone;
  }
  if (!resume) {
    rawDone = streamOffset = 0;
    Storage.remove(FIRMWARE_TEMP);
    Storage.remove(FIRMWARE_META);
  }
  output = Storage.open(FIRMWARE_TEMP, O_RDWR | O_CREAT);
  if (!output || !assembler.begin(*this, size, digest, rawDone, streamOffset)) {
    failure = output ? assembler.error() : Error::StorageFailure;
    closeFiles();
    return false;
  }
  if (resume) {
    // The whole-image hash continues over the bytes already on the card;
    // a block written after the checkpoint (power loss mid-block) is
    // overwritten from the checkpoint on.
    size_t left = rawDone;
    while (left) {
      const size_t count = left < sizeof(block) ? left : sizeof(block);
      if (output.read(block, count) != static_cast<int>(count)) {
        failure = Error::StorageFailure;
        closeFiles();
        return false;
      }
      assembler.hashPrefix(block, count);
      left -= count;
    }
  }
  if (!output.seek(rawDone)) {
    failure = Error::StorageFailure;
    closeFiles();
    return false;
  }
  running = true;
  LOG_INF("FWBLE", "Receiving firmware %u bytes, resume at %u (stream %u)", (unsigned)size, (unsigned)rawDone,
          (unsigned)streamOffset);
  return true;
}

bool FirmwareReceiver::feed(const uint8_t* data, const size_t length) {
  if (!running || failure != Error::None) return false;
  if (!assembler.feed(data, length)) {
    if (assembler.error() == Error::ChecksumMismatch) {
      // The image cannot be trusted: no resume from it.
      closeFiles();
      Storage.remove(FIRMWARE_TEMP);
      Storage.remove(FIRMWARE_META);
    }
    return false;
  }
  if (assembler.phase() == firmware_v4::Assembler::Phase::Complete) {
    closeFiles();
    running = false;
  }
  return true;
}

bool FirmwareReceiver::write(const uint8_t* data, const size_t length) {
  return output && output.write(data, length) == length;
}

bool FirmwareReceiver::stageBegin() {
  if (!stage) {
    stage = Storage.open(BLOCK_STAGE, O_RDWR | O_CREAT | O_TRUNC);
    if (!stage) return false;
  }
  stageBytes = stageReadAt = 0;
  return stage.seek(0);
}

bool FirmwareReceiver::stageWrite(const uint8_t* data, const size_t length) {
  if (!stage || stage.write(data, length) != length) return false;
  stageBytes += length;
  return true;
}

bool FirmwareReceiver::stageRewind() {
  if (!stage) return false;
  stage.flush();
  stageReadAt = 0;
  return stage.seek(0);
}

int FirmwareReceiver::stageRead(uint8_t* data, const size_t length) {
  if (!stage) return -1;
  const size_t count = length < stageBytes - stageReadAt ? length : stageBytes - stageReadAt;
  if (count == 0) return 0;
  const int read = stage.read(data, count);
  if (read != static_cast<int>(count)) return -1;
  stageReadAt += count;
  return read;
}

bool FirmwareReceiver::checkpoint(const size_t rawDone, const size_t streamOffset) {
  // The block is on the card before the checkpoint names it.
  output.flush();
  HalFile meta;
  if (!Storage.openFileForWrite("FWBLE", FIRMWARE_META, meta)) return false;
  char text[160];
  const int length = snprintf(text, sizeof(text), "%s %u %u %u", sha.c_str(), (unsigned)size, (unsigned)rawDone,
                              (unsigned)streamOffset);
  const bool ok = length > 0 && meta.write(reinterpret_cast<const uint8_t*>(text), length) == size_t(length);
  meta.close();
  return ok;
}
