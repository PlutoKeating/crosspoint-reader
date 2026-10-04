#include "StudioReceiver.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "StudioFrame.h"
#ifndef SIMULATOR
#include <Arduino.h>
#endif

using studio_v4::Digest;
using studio_v4::Error;
using studio_v4::FRAME_BYTES;

namespace {
constexpr const char* TEMP = "/.crosspoint/studio/incoming.bin";
constexpr const char* RECORD_STAGE = "/.crosspoint/studio/record.z";
constexpr size_t MAX_HEADER_JSON = 32768;
// One block buffer for frame copies and header reads; only the writer task
// runs the receiver, so it is static instead of on that task's stack.
uint8_t block[512];

bool heapAllows(size_t bytes) {
#ifndef SIMULATOR
  return ESP.getMaxAllocHeap() >= bytes + 4096;
#else
  (void)bytes;
  return true;
#endif
}
}  // namespace

StudioReceiver& StudioReceiver::instance() {
  static StudioReceiver receiver;
  return receiver;
}

StudioReceiver::Resume StudioReceiver::resumeFor(size_t header, size_t size, size_t partialBytes) {
  Resume resume;
  if (header == 0) {
    // A single frame: either fully on the card (commit without data) or restarted.
    resume.framesDone = partialBytes >= FRAME_BYTES ? 1 : 0;
    return resume;
  }
  if (partialBytes < header || size < header) return resume;
  resume.headerDone = true;
  resume.offset = header;  // the phone regenerates the records of the remaining `need` from here
  resume.framesDone = std::min((partialBytes - header) / FRAME_BYTES, (size - header) / FRAME_BYTES);
  return resume;
}

bool StudioReceiver::start(const std::string& task, const std::string& transferHash, int64_t expires,
                           size_t transferSize, size_t transferHeader, const Resume& resume) {
  abort(false);
  failure = Error::None;
  hash = transferHash;
  header = transferHeader;
  size = transferSize;
  sources.clear();
  sourcePaths.clear();
  const size_t limit = header == 0 ? resume.framesDone * FRAME_BYTES
                       : resume.headerDone ? header + resume.framesDone * FRAME_BYTES
                                           : 0;
  auto& frame = StudioFrame::instance();
  if (!frame.start(task, hash, expires, size, limit)) {
    failure = Error::InsufficientStorage;
    return false;
  }
  running = true;
  if (frame.received() != limit) {
    // The card holds less than begin4 promised (a partial went missing):
    // the phone's retry gets the real resume point.
    LOG_ERR("STUDIO", "Resume point moved (%u != %u)", (unsigned)frame.received(), (unsigned)limit);
    failure = Error::OffsetMismatch;
    return false;
  }
  Digest single{};
  if (header == 0 && !studio_v4::parseDigest(hash.c_str(), single)) {
    failure = Error::FrameValidation;
    return false;
  }
  if (!assembler.begin(*this, header, size, header == 0 ? &single : nullptr, resume.framesDone, resume.headerDone)) {
    LOG_ERR("STUDIO", "Transfer setup failed: %s", studio_v4::errorName(assembler.error()));
    return false;
  }
  LOG_INF("STUDIO", "Receiving %u bytes (header %u, resume %u frames), need %u", (unsigned)size, (unsigned)header,
          (unsigned)resume.framesDone, (unsigned)assembler.needCount());
  return true;
}

size_t StudioReceiver::rebuiltBytes() const { return running ? StudioFrame::instance().received() : 0; }

bool StudioReceiver::feed(const uint8_t* data, size_t length) {
  if (!running || failure != Error::None) return false;
  return assembler.feed(data, length);
}

bool StudioReceiver::commit() {
  if (!running) return false;
  if (assembler.phase() != studio_v4::Assembler::Phase::Complete) {
    failure = Error::FrameValidation;
    return false;
  }
  closeStage();
  running = false;
  if (!StudioFrame::instance().commit()) {
    failure = Error::FrameValidation;
    return false;
  }
  return true;
}

void StudioReceiver::abort(bool discard) {
  closeStage();
  if (running) StudioFrame::instance().abort(discard);
  running = false;
  assembler.reset();
}

void StudioReceiver::closeStage() {
  if (stage) stage.close();
  stageBytes = stageReadAt = 0;
}

bool StudioReceiver::write(const uint8_t* data, size_t length) {
  auto& frame = StudioFrame::instance();
  return frame.append(frame.received(), data, length);
}

// SSP1 header -> per-frame digests (`frames[].sha256`), filtered so only the
// digests are kept in the JSON document.
bool StudioReceiver::readDigests(const std::string& path, size_t fileSize, std::vector<Digest>& out,
                                 size_t& headerBytes, Error& error) {
  error = Error::FrameValidation;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) {
    error = Error::StorageFailure;
    return false;
  }
  uint8_t prefix[8];
  if (file.read(prefix, 8) != 8 || memcmp(prefix, "SSP1", 4) != 0) return false;
  const size_t length =
      size_t(prefix[4]) | size_t(prefix[5]) << 8 | size_t(prefix[6]) << 16 | size_t(prefix[7]) << 24;
  if (length == 0 || length > MAX_HEADER_JSON || 8 + length > fileSize) return false;
  if (!heapAllows(length)) {
    error = Error::DeviceBusy;
    return false;
  }
  auto json = makeUniqueNoThrow<char[]>(length);
  if (!json) {
    error = Error::DeviceBusy;
    return false;
  }
  if (file.read(json.get(), length) != static_cast<int>(length)) {
    error = Error::StorageFailure;
    return false;
  }
  file.close();
  JsonDocument filter;
  filter["frames"][0]["sha256"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json.get(), length, DeserializationOption::Filter(filter))) return false;
  JsonArrayConst frames = doc["frames"].as<JsonArrayConst>();
  if (frames.size() == 0) return false;
  if (frames.size() > studio_v4::MAX_FRAMES) {
    error = Error::TooManyFrames;
    return false;
  }
  if (!heapAllows(frames.size() * sizeof(Digest))) {
    error = Error::DeviceBusy;
    return false;
  }
  out.clear();
  out.reserve(frames.size());
  for (JsonVariantConst f : frames) {
    Digest d{};
    if (!studio_v4::parseDigest(f["sha256"].as<const char*>(), d)) return false;
    out.push_back(d);
  }
  headerBytes = 8 + length;
  return true;
}

bool StudioReceiver::resolve(std::vector<Digest>& frames, std::vector<bool>& have, Error& error) {
  std::vector<Digest> local;
  if (header == 0) {
    Digest single{};
    studio_v4::parseDigest(hash.c_str(), single);
    local.assign(1, single);
    frames.clear();
  } else {
    auto& frame = StudioFrame::instance();
    if (!frame.flushOutput()) {
      error = Error::StorageFailure;
      return false;
    }
    size_t headerBytes = 0;
    if (!readDigests(TEMP, size, local, headerBytes, error)) return false;
    if (headerBytes != header) {
      error = Error::FrameValidation;
      return false;
    }
    frames = local;
  }
  have.assign(local.size(), false);
  sources.assign(local.size(), Source{});
  sourcePaths.clear();
  // Look the digests up in every kept file; the first holder wins.
  for (const auto& kept : StudioFrame::instance().keptFiles()) {
    const std::string path = StudioFrame::fileFor(kept.hash);
    std::vector<Digest> held;
    size_t keptHeader = 0;
    if (kept.size == FRAME_BYTES) {
      Digest d{};
      if (!studio_v4::parseDigest(kept.hash.c_str(), d)) continue;
      held.assign(1, d);
    } else {
      Error ignored = Error::None;
      if (!readDigests(path, kept.size, held, keptHeader, ignored) ||
          keptHeader + held.size() * FRAME_BYTES != kept.size)
        continue;
    }
    bool used = false;
    for (size_t i = 0; i < local.size(); ++i) {
      if (have[i]) continue;
      for (size_t j = 0; j < held.size(); ++j) {
        if (memcmp(held[j].data(), local[i].data(), 32) != 0) continue;
        have[i] = true;
        sources[i] = {static_cast<uint8_t>(sourcePaths.size()), static_cast<uint32_t>(keptHeader + j * FRAME_BYTES)};
        used = true;
        break;
      }
    }
    if (used) sourcePaths.push_back(path);
  }
  LOG_INF("STUDIO", "Resolved %u frames, %u held locally", (unsigned)local.size(),
          (unsigned)std::count(have.begin(), have.end(), true));
  return true;
}

bool StudioReceiver::copyFrame(size_t index) {
  if (index >= sources.size() || sources[index].file >= sourcePaths.size()) return false;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", sourcePaths[sources[index].file], file) ||
      !file.seek(sources[index].offset))
    return false;
  size_t remaining = FRAME_BYTES;
  while (remaining) {
    const size_t count = std::min(remaining, sizeof(block));
    if (file.read(block, count) != static_cast<int>(count) || !write(block, count)) return false;
    remaining -= count;
  }
  return true;
}

bool StudioReceiver::stageBegin() {
  if (!stage) {
    stage = Storage.open(RECORD_STAGE, O_RDWR | O_CREAT | O_TRUNC);
    if (!stage) return false;
  }
  stageBytes = stageReadAt = 0;
  return stage.seek(0);
}

bool StudioReceiver::stageWrite(const uint8_t* data, size_t length) {
  if (!stage || stage.write(data, length) != length) return false;
  stageBytes += length;
  return true;
}

bool StudioReceiver::stageRewind() {
  if (!stage) return false;
  stage.flush();
  stageReadAt = 0;
  return stage.seek(0);
}

int StudioReceiver::stageRead(uint8_t* data, size_t length) {
  if (!stage) return -1;
  const size_t count = std::min(length, stageBytes - stageReadAt);
  if (count == 0) return 0;
  const int read = stage.read(data, count);
  if (read != static_cast<int>(count)) return -1;
  stageReadAt += count;
  return read;
}
