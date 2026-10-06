#include "StudioReceiver.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "FrameStore.h"
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
  // The digests and the inflater are only needed while receiving (2.7.2:
  // nothing of a transfer stays on the heap once it is installed).
  assembler.reset();
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
bool StudioReceiver::readDigests(const std::string& path, size_t start, size_t bound, std::vector<Digest>& out,
                                 size_t& headerBytes, Error& error) {
  error = Error::FrameValidation;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file) || !file.seek(start)) {
    error = Error::StorageFailure;
    return false;
  }
  uint8_t prefix[8];
  if (file.read(prefix, 8) != 8 || memcmp(prefix, "SSP1", 4) != 0) return false;
  const size_t length =
      size_t(prefix[4]) | size_t(prefix[5]) << 8 | size_t(prefix[6]) << 16 | size_t(prefix[7]) << 24;
  if (length == 0 || length > MAX_HEADER_JSON || 8 + length > bound) return false;
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
  auto& frame = StudioFrame::instance();
  if (header == 0) {
    Digest single{};
    studio_v4::parseDigest(hash.c_str(), single);
    local.assign(1, single);
    frames.clear();
  } else {
    std::string path;
    size_t start = 0, headerBytes = 0;
    if (!frame.incomingHeader(path, start)) {
      error = Error::StorageFailure;
      return false;
    }
    if (!readDigests(path, start, size, local, headerBytes, error)) return false;
    if (headerBytes != header) {
      error = Error::FrameValidation;
      return false;
    }
    frames = local;
  }
  have.assign(local.size(), false);
  sources.assign(local.size(), Source{});
  sourcePaths.clear();
  // Frames already in a slot (2.7.10): the new program references them.
  size_t inSlots = 0;
  if (frame.slotsActive()) {
    for (size_t i = 0; i < local.size(); ++i) {
      const uint16_t slot = frame.findSlot(local[i]);
      if (slot == frame_slots::NO_SLOT) continue;
      have[i] = true;
      sources[i].slot = slot;
      ++inSlots;
    }
  }
  // Then every kept program or frame: a 2.7.9 whole file (copied once into a
  // slot, or into incoming.bin), or a record whose frames a whole-file
  // transfer copies from the slot area. The first holder wins.
  for (const auto& kept : frame.keptFiles()) {
    if (std::find(have.begin(), have.end(), false) == have.end()) break;
    std::vector<Digest> held;
    size_t keptHeader = 0;
    if (kept.size == FRAME_BYTES) {
      Digest d{};
      if (!studio_v4::parseDigest(kept.hash.c_str(), d)) continue;
      held.assign(1, d);
    } else {
      std::string path;
      size_t start = 0;
      Error ignored = Error::None;
      if (!frame_store::headerOf(kept.hash, kept.size, path, start) ||
          !readDigests(path, start, kept.size, held, keptHeader, ignored) ||
          keptHeader + held.size() * FRAME_BYTES != kept.size)
        continue;
    }
    for (size_t i = 0; i < local.size(); ++i) {
      if (have[i]) continue;
      for (size_t j = 0; j < held.size(); ++j) {
        if (memcmp(held[j].data(), local[i].data(), 32) != 0) continue;
        std::string path;
        size_t at = 0;
        if (!frame_store::locate(kept.hash, kept.size, keptHeader + j * FRAME_BYTES, path, at)) break;
        auto known = std::find(sourcePaths.begin(), sourcePaths.end(), path);
        if (known == sourcePaths.end()) {
          if (sourcePaths.size() >= 255) break;
          sourcePaths.push_back(path);
          known = sourcePaths.end() - 1;
        }
        have[i] = true;
        sources[i] = {static_cast<uint8_t>(known - sourcePaths.begin()), static_cast<uint32_t>(at)};
        break;
      }
    }
  }
  LOG_INF("STUDIO", "Resolved %u frames, %u held locally (%u in slots)", (unsigned)local.size(),
          (unsigned)std::count(have.begin(), have.end(), true), (unsigned)inSlots);
  return true;
}

bool StudioReceiver::copyFrame(size_t index) {
  if (index >= sources.size()) return false;
  if (sources[index].slot != frame_slots::NO_SLOT) return StudioFrame::instance().appendSlot(sources[index].slot);
  if (sources[index].file >= sourcePaths.size()) return false;
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
