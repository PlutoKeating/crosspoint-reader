#include "StudioFrame.h"

#include <atomic>

#include <ArduinoJson.h>
#include <CardStrip.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "FrameStore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <new>

#ifndef SIMULATOR
#include <Arduino.h>
#endif

namespace {
constexpr const char* ROOT = "/.crosspoint/studio";
constexpr const char* TEMP = "/.crosspoint/studio/incoming.bin";
constexpr const char* PARTIAL = "/.crosspoint/studio/incoming.json";
constexpr const char* STATE = "/.crosspoint/studio/state.json";
constexpr const char* BACKUP = "/.crosspoint/studio/state.bak";
constexpr const char* STATE_TEMP = "/.crosspoint/studio/state.tmp";
constexpr const char* RECORD_STAGE = "/.crosspoint/studio/record.z";
bool heapAllows(size_t bytes) {
#ifndef SIMULATOR
  return ESP.getMaxAllocHeap() >= bytes + 8192;
#else
  (void)bytes;
  return true;
#endif
}
bool validHash(const std::string& hash) {
  return hash.size() == 64 &&
         std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}  // namespace

std::string StudioFrame::fileFor(const std::string& hash) { return frame_store::fileFor(hash); }
StudioFrame& StudioFrame::instance() {
  static StudioFrame frame;
  return frame;
}
class StudioFrame::Writer {
 public:
  explicit Writer(StudioFrame& frame) : frame_(frame), lock_(frame.mutex) {}
  ~Writer() { frame_.publishLocked(); }  // runs before lock_ is released

 private:
  StudioFrame& frame_;
  std::lock_guard<std::recursive_mutex> lock_;
};
void StudioFrame::publishLocked() {
  std::lock_guard<std::mutex> view(viewMutex);
  activeView = active;
  partialTaskView = partial.task;
  partialHashView = partial.hash;
  partialSizeView = partial.size;
  partialBytesView.store(partial.bytes);
  offsetView.store(offset);
  receivingView.store(receiving);
  hasContentView.store(!active.hash.empty());
}
void StudioFrame::load() {
  Writer lock(*this);
  if (loaded) return;
  loaded = true;
  Storage.mkdir(ROOT, true);
  {
    // Remember the resumable partial, if any, for resumeOffset().
    HalFile metadata;
    if (Storage.openFileForRead("STUDIO", PARTIAL, metadata)) {
      JsonDocument doc;
      const bool valid = !deserializeJson(doc, metadata);
      metadata.close();
      HalFile temp;
      if (valid && (doc["slots"] | false)) {
        partial = {doc["task"] | "", doc["hash"] | "", doc["size"] | size_t(0), frame_store::Incoming::partialBytes()};
      } else if (valid && Storage.openFileForRead("STUDIO", TEMP, temp)) {
        partial = {doc["task"] | "", doc["hash"] | "", doc["size"] | size_t(0), temp.size()};
        temp.close();
      }
    }
  }
  for (const char* path : {STATE, BACKUP}) {
    HalFile input;
    if (!Storage.openFileForRead("STUDIO", path, input)) continue;
    JsonDocument doc;
    const auto error = deserializeJson(doc, input);
    input.close();
    if (error) continue;
    const std::string hash = doc["hash"] | "";
    if (!validHash(hash)) continue;
    // The active file, the last visual and the saved program are usually the
    // same (multi-megabyte) file: hash each distinct file once at boot.
    std::string verified;
    auto verifiedFile = [&verified](const std::string& h, size_t size) {
      if (h == verified) return true;
      if (!frame_store::verify(h, size)) return false;
      verified = h;
      return true;
    };
    if (!verifiedFile(hash, doc["size"] | BYTES)) continue;
    active = {doc["task"] | "", hash, doc["expires"] | int64_t(0), false, doc["size"] | BYTES, doc["card"] | ""};
    auto visual = doc["visual"];
    lastVisual = {visual["task"] | "", visual["hash"] | "", 0, false, visual["size"] | BYTES, ""};
    lastVisualOffset = visual["offset"] | size_t(0);
    if (!validHash(lastVisual.hash) || !verifiedFile(lastVisual.hash, lastVisual.size) ||
        lastVisualOffset + BYTES > lastVisual.size)
      lastVisual = {};
    auto saved = doc["program"];
    savedProgram = {saved["task"] | "", saved["hash"] | "", 0, false, saved["size"] | BYTES, ""};
    if (active.size > BYTES) savedProgram = active;
    if (!savedProgram.hash.empty() && validHash(savedProgram.hash) &&
        verifiedFile(savedProgram.hash, savedProgram.size) && readProgram(savedProgram, program, headerOffset)) {
      auto state = doc["playback"];
      playback.lastTime = state["time"] | int64_t(0);
      playback.lastChange = state["change"] | int64_t(0);
      playback.manualUntil = state["until"] | int64_t(-1);
      playback.scene = state["scene"] | "";
      playback.lastCard = state["card"] | "";
      playback.index = std::max(0, state["index"] | 0);
      playback.manual = state["manual"] | false;
      if (active.size > BYTES) {
        selectedFrame = studio::step(program, playback, playback.lastTime);
        pixelOffset = headerOffset + (selectedFrame < 0 ? 0 : selectedFrame) * BYTES;
      }
    } else {
      savedProgram = {};
      program = {};
      if (active.size > BYTES) {
        active = {};
        continue;
      }
    }
    ++revision;
    break;
  }
}
namespace {
// Written by refreshStorageUsage() (worker task), read anywhere; a torn read of
// a stale pair only affects a capacity estimate.
std::atomic<uint64_t> usageTotal{0}, usageUsed{0};
std::atomic<bool> usageValid{false};
}  // namespace
bool StudioFrame::storageUsage(uint64_t& total, uint64_t& used) {
  total = usageTotal.load(std::memory_order_relaxed);
  used = usageUsed.load(std::memory_order_relaxed);
  return usageValid.load(std::memory_order_acquire);
}
void StudioFrame::refreshStorageUsage() {
  const uint64_t total = Storage.totalBytes();
  const uint64_t used = total ? Storage.usedBytes() : 0;
  usageTotal.store(total, std::memory_order_relaxed);
  usageUsed.store(used, std::memory_order_relaxed);
  usageValid.store(total > 0, std::memory_order_release);
}
bool StudioFrame::start(const std::string& task, const std::string& hash, int64_t expires, size_t size,
                        size_t resumeLimit) {
  Writer lock(*this);
  if (receiving || task.size() != 36 || !validHash(hash) || size < BYTES || size > MAX_BYTES) return false;
  Storage.mkdir(ROOT, true);
  incoming = {task, hash, expires, false, size, ""};
  offset = 0;
  bool resume = false, resumeSlots = false;
  HalFile metadata;
  if (Storage.openFileForRead("STUDIO", PARTIAL, metadata)) {
    JsonDocument doc;
    const bool valid = !deserializeJson(doc, metadata);
    metadata.close();
    resume = valid && std::string(doc["task"] | "") == task && std::string(doc["hash"] | "") == hash &&
             (doc["size"] | size_t(0)) == size;
    resumeSlots = resume && (doc["slots"] | false);
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  // Frame slots first (2.7.10): frames go to the preallocated area, the
  // header to incoming.ssp. Without the area, or without enough slots no kept
  // program uses, the transfer is one whole file as before.
  // ~5.5 KB (the index) for the duration of the transfer, only when the heap
  // has it with room to spare; otherwise the whole-file path.
  // A whole-file partial of this transfer (2.7.9, or no room last time) is
  // resumed as a whole file.
  if ((!resume || resumeSlots) && heapAllows(sizeof(frame_store::Incoming)))
    slotsIn.reset(new (std::nothrow) frame_store::Incoming);
  if (slotsIn && slotsIn->begin(size, keepListLocked(), resumeSlots, resumeLimit, sha, offset)) {
    if (!resumeSlots) Storage.remove(TEMP);
  } else {
    slotsIn.reset();
    offset = 0;
    mbedtls_sha256_starts(&sha, 0);
    Storage.remove(frame_store::RECORD_TEMP);
    if (resumeSlots) resume = false;  // its frames sit in slots this transfer cannot use
    if (resume) {
      HalFile file;
      if (Storage.openFileForRead("STUDIO", TEMP, file) && file.size() <= size) {
        const size_t limit = std::min<size_t>(file.size(), resumeLimit);
        static uint8_t chunk[512];  // off the caller's stack (writer task / UI loop)
        while (offset < limit) {
          const int count = file.read(chunk, std::min(sizeof(chunk), limit - offset));
          if (count <= 0) {
            resume = false;
            break;
          }
          mbedtls_sha256_update(&sha, chunk, count);
          offset += count;
        }
        file.close();
      } else
        resume = false;
    }
    if (!resume) {
      offset = 0;
      mbedtls_sha256_starts(&sha, 0);
      Storage.remove(TEMP);
      partial = {};
    }
    // Capacity check from the worker's last reading: a scan here would stall the
    // main loop (and the chunk queue) for seconds right after `begin`. Without a
    // reading the write itself reports a full card. (The slot area is
    // preallocated: nothing to check there.)
    uint64_t total = 0, used = 0;
    if (storageUsage(total, used) && (total <= used || total - used < size - offset + 65536)) {
      mbedtls_sha256_free(&sha);
      return false;
    }
    // Not O_APPEND: a resume limited below the partial's length overwrites the
    // tail (the same task and hash always produce the same bytes there).
    output = Storage.open(TEMP, O_WRONLY | O_CREAT);
    if (!output || !output.seek(offset)) {
      output.close();
      mbedtls_sha256_free(&sha);
      return false;
    }
  }
  JsonDocument doc;
  doc["task"] = task;
  doc["hash"] = hash;
  doc["size"] = size;
  doc["slots"] = slotsIn != nullptr;
  HalFile meta;
  bool saved = Storage.openFileForWrite("STUDIO", PARTIAL, meta);
  saved = saved && serializeJson(doc, meta) > 0;
  meta.close();
  if (!saved) {
    closeIncomingLocked();
    mbedtls_sha256_free(&sha);
    return false;
  }
  receiving = true;
  partial = {task, hash, size, offset};
  return true;
}
size_t StudioFrame::resumeOffset(const std::string& task, const std::string& hash, const size_t size) const {
  std::lock_guard<std::mutex> view(viewMutex);
  const size_t bytes = partialBytesView.load();
  return partialTaskView == task && partialHashView == hash && partialSizeView == size && bytes <= size ? bytes : 0;
}
bool StudioFrame::append(size_t expectedOffset, const uint8_t* data, size_t length) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || expectedOffset != offset || length > incoming.size - offset || !length) return false;
  if (slotsIn ? !slotsIn->write(offset, data, length) : output.write(data, length) != length) {
    abort();
    return false;
  }
  mbedtls_sha256_update(&sha, data, length);
  offset += length;
  partial.bytes = offset;
  offsetView.store(offset);
  partialBytesView.store(offset);
  return true;
}
bool StudioFrame::slotsActive() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return receiving && slotsIn;
}
uint16_t StudioFrame::findSlot(const std::array<uint8_t, 32>& digest) const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return receiving && slotsIn ? slotsIn->find(digest) : frame_slots::NO_SLOT;
}
bool StudioFrame::appendSlot(const uint16_t slot) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || !slotsIn || incoming.size - offset < BYTES) return false;
  if (!slotsIn->reference(offset, slot, sha)) {
    abort();
    return false;
  }
  offset += BYTES;
  partial.bytes = offset;
  offsetView.store(offset);
  partialBytesView.store(offset);
  return true;
}
bool StudioFrame::incomingHeader(std::string& path, size_t& start) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving) return false;
  if (slotsIn) return slotsIn->headerSource(path, start);
  output.flush();
  path = TEMP;
  start = 0;
  return true;
}
void StudioFrame::closeIncomingLocked() {
  if (slotsIn) {
    slotsIn->close();
    slotsIn.reset();
  } else {
    output.close();
  }
}
std::vector<std::string> StudioFrame::keepListLocked() const {
  std::vector<std::string> keep{active.hash, savedProgram.hash, lastVisual.hash};
  HalFile backup;
  if (Storage.openFileForRead("STUDIO", BACKUP, backup)) {
    JsonDocument doc;
    if (!deserializeJson(doc, backup)) {
      keep.emplace_back(doc["hash"] | "");
      keep.emplace_back(doc["program"]["hash"] | "");
      keep.emplace_back(doc["visual"]["hash"] | "");
    }
    backup.close();
  }
  return keep;
}
bool StudioFrame::commit() {
  // Three steps; the card work between them runs without the lock, so the UI
  // loop and the BLE callbacks reading the published state never wait for a
  // rename, a header parse or the garbage collection (2.7.10).
  Snapshot next;
  bool slotted = false;
  {
    Writer lock(*this);
    if (!receiving || offset != incoming.size) {
      abort();
      return false;
    }
    uint8_t bytes[32];
    mbedtls_sha256_finish(&sha, bytes);
    mbedtls_sha256_free(&sha);
    receiving = false;
    slotted = slotsIn != nullptr;
    closeIncomingLocked();
    partial = {};
    char digest[65];
    for (size_t i = 0; i < 32; ++i) snprintf(digest + i * 2, 3, "%02x", bytes[i]);
    if (incoming.hash != digest) {
      Storage.remove(TEMP);
      Storage.remove(frame_store::RECORD_TEMP);
      Storage.remove(PARTIAL);
      return false;
    }
    next = incoming;
  }
  Storage.remove(PARTIAL);
  Storage.remove(RECORD_STAGE);
  // A record (slots) or a whole file, under the content's hash.
  const char* temp = slotted ? frame_store::RECORD_TEMP : TEMP;
  const std::string path = slotted ? frame_store::recordFor(next.hash) : fileFor(next.hash);
  if (Storage.exists(path.c_str()) && frame_store::verify(next.hash, next.size))
    Storage.remove(temp);
  else {
    Storage.remove(path.c_str());
    if (!Storage.rename(temp, path.c_str())) return false;
  }
  studio::Program nextProgram;
  size_t nextHeader = 0;
  if (next.size > BYTES && !readProgram(next, nextProgram, nextHeader)) {
    Storage.remove(path.c_str());
    return false;
  }
  {
    Writer lock(*this);
    const Snapshot old = active, oldSaved = savedProgram, oldVisual = lastVisual;
    const auto oldVisualOffset = lastVisualOffset;
    const int oldSelected = selectedFrame, oldAlert = alertFrame;
    const bool oldAlertDisplayed = alertDisplayed;
    const auto oldProgram = program;
    const auto oldPlayback = playback;
    const size_t oldOffset = pixelOffset, oldHeader = headerOffset;
    if (!active.hash.empty() && (active.size == BYTES || selectedFrame >= 0)) {
      lastVisual = active;
      lastVisualOffset = pixelOffset;
    }
    active = next;
    alertFrame = -1;
    alertDisplayed = false;
    pixelOffset = 0;
    if (next.size > BYTES) {
      savedProgram = next;
      program = std::move(nextProgram);
      headerOffset = nextHeader;
      playback = {};
      lastTickNow = 0;
      selectedFrame = -1;
      pixelOffset = headerOffset;
    }
    if (!persist()) {
      active = old;
      savedProgram = oldSaved;
      program = oldProgram;
      playback = oldPlayback;
      lastTickNow = 0;
      pixelOffset = oldOffset;
      headerOffset = oldHeader;
      lastVisual = oldVisual;
      lastVisualOffset = oldVisualOffset;
      selectedFrame = oldSelected;
      alertFrame = oldAlert;
      alertDisplayed = oldAlertDisplayed;
      return false;
    }
    ++revision;
  }
  collectGarbage();
  return true;
}
void StudioFrame::abort(bool discard) {
  Writer lock(*this);
  if (receiving) {
    closeIncomingLocked();
    mbedtls_sha256_free(&sha);
    receiving = false;
  }
  if (discard) {
    Storage.remove(TEMP);
    Storage.remove(frame_store::RECORD_TEMP);
    Storage.remove(PARTIAL);
    Storage.remove(RECORD_STAGE);
    offset = 0;
    partial = {};
  }
}
bool StudioFrame::persist() {
  JsonDocument doc;
  doc["task"] = active.task;
  doc["hash"] = active.hash;
  doc["expires"] = active.expires;
  doc["size"] = active.size;
  doc["card"] = active.card;
  auto visual = doc["visual"].to<JsonObject>();
  visual["task"] = lastVisual.task;
  visual["hash"] = lastVisual.hash;
  visual["size"] = lastVisual.size;
  visual["offset"] = lastVisualOffset;
  auto saved = doc["program"].to<JsonObject>();
  saved["task"] = savedProgram.task;
  saved["hash"] = savedProgram.hash;
  saved["size"] = savedProgram.size;
  auto state = doc["playback"].to<JsonObject>();
  state["time"] = playback.lastTime;
  state["change"] = playback.lastChange;
  state["until"] = playback.manualUntil;
  state["scene"] = playback.scene;
  state["card"] = playback.lastCard;
  state["index"] = playback.index;
  state["manual"] = playback.manual;
  // A document that overflowed its allocator would parse later with members
  // missing; a short write would not parse at all. Neither replaces the state.
  if (doc.overflowed()) return false;
  const size_t expected = measureJson(doc);
  Storage.remove(STATE_TEMP);
  HalFile file;
  if (!Storage.openFileForWrite("STUDIO", STATE_TEMP, file)) return false;
  const bool written = serializeJson(doc, file) == expected;
  file.close();
  if (!written) {
    Storage.remove(STATE_TEMP);
    return false;
  }
  Storage.remove(BACKUP);
  if (Storage.exists(STATE) && !Storage.rename(STATE, BACKUP)) return false;
  if (!Storage.rename(STATE_TEMP, STATE)) {
    if (Storage.exists(BACKUP)) Storage.rename(BACKUP, STATE);
    return false;
  }
  return true;
}
bool StudioFrame::visualSource(std::string& path, size_t& start) const {
  std::string hash;
  size_t size = 0, at = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (active.hash.empty()) return false;
    const bool hold = active.size > BYTES && selectedFrame < 0;
    const auto& visual = alertFrame >= 0 ? savedProgram : hold ? lastVisual : active;
    at = alertFrame >= 0 ? headerOffset + size_t(alertFrame) * BYTES : hold ? lastVisualOffset : pixelOffset;
    if (visual.hash.empty() || at + BYTES > visual.size) return false;
    hash = visual.hash;
    size = visual.size;
  }
  // Card reads (the record's slot table) happen without the lock.
  return frame_store::locate(hash, size, at, path, start);
}
bool StudioFrame::render(const GfxRenderer& renderer) {
  // Only the choice of file and offset needs the lock. The SD read and the
  // pixel loop below take hundreds of milliseconds on the render task; holding
  // the lock through them stalled the UI loop (frame.tick, hasContent) and
  // with it button sampling, so a key released during a repaint was lost.
  std::string path;
  size_t start = 0;
  if (renderer.getScreenWidth() != WIDTH || renderer.getScreenHeight() != HEIGHT) return false;
  if (!visualSource(path, start)) return false;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) return false;
  if (start + BYTES > file.size() || !file.seek(start)) {
    file.close();
    return false;
  }
  renderer.clearScreen();
  uint8_t row[WIDTH / 8];
  for (size_t y = 0; y < HEIGHT; ++y) {
    if (file.read(row, sizeof(row)) != sizeof(row)) {
      file.close();
      return false;
    }
    for (size_t x = 0; x < WIDTH; ++x)
      if (row[x / 8] & (0x80 >> (x & 7))) renderer.drawPixel(x, y, true);
  }
  file.close();
  return true;
}
namespace {
struct StripStream {
  HalFile file;
  size_t start = 0;
  const GfxRenderer* renderer = nullptr;
  StudioFrame::Overlay overlay = nullptr;
  void* overlayCtx = nullptr;
  uint8_t* group = nullptr;  // eight card rows
};
bool fillStrip(uint8_t* strip, const uint16_t x0, const uint16_t cols, void* ctx) {
  auto& s = *static_cast<StripStream*>(ctx);
  const uint16_t stripBytes = cols / 8;
  if (!s.file.seek(s.start + size_t(x0) * card_strip::CARD_ROW_BYTES)) return false;
  for (uint16_t k = 0; k < stripBytes; ++k) {
    if (s.file.read(s.group, card_strip::GROUP_BYTES) != static_cast<int>(card_strip::GROUP_BYTES)) return false;
    card_strip::transposeGroup(s.group, strip, stripBytes, k);
  }
  if (s.overlay) {
    s.renderer->beginColumnStripTarget(strip, x0, cols);
    s.overlay(*s.renderer, s.overlayCtx);
    s.renderer->endStripTarget();
  }
  return true;
}
}  // namespace
StudioFrame::StreamResult StudioFrame::stream(const GfxRenderer& renderer, const Overlay overlay, void* ctx,
                                              const HalDisplay::RefreshMode mode) {
  using card_strip::CARD_WIDTH;
  if (!renderer.supportsStripDisplay() || renderer.getOrientation() != GfxRenderer::Portrait ||
      renderer.getDisplayWidth() != HEIGHT || renderer.getDisplayHeight() != WIDTH)
    return StreamResult::Unavailable;
  std::string path;
  size_t start = 0;
  if (!visualSource(path, start)) return StreamResult::Unavailable;
  StripStream s;
  if (!Storage.openFileForRead("STUDIO", path, s.file)) return StreamResult::Unavailable;
  if (start + BYTES > s.file.size()) {
    s.file.close();
    return StreamResult::Unavailable;
  }
  // Strips of 72 panel columns (11 per frame, 4.7 KB); narrower ones when the
  // heap is tight. The eight-row read buffer rides in the same block.
  uint8_t* block = nullptr;
  uint16_t stripCols = 0;
  for (const uint16_t cols : {uint16_t(72), uint16_t(24), uint16_t(8)}) {
    block = static_cast<uint8_t*>(malloc(size_t(CARD_WIDTH) * cols / 8 + card_strip::GROUP_BYTES));
    if (block) {
      stripCols = cols;
      break;
    }
  }
  if (!block) {
    s.file.close();
    return StreamResult::Unavailable;
  }
  s.start = start;
  s.renderer = &renderer;
  s.overlay = overlay;
  s.overlayCtx = ctx;
  s.group = block + size_t(CARD_WIDTH) * stripCols / 8;
  const bool ok = renderer.displayStrips(&fillStrip, &s, block, stripCols, mode);
  free(block);
  s.file.close();
  if (ok)
    LOG_DBG("STUDIO", "Streamed card in %u-column strips", static_cast<unsigned>(stripCols));
  else
    LOG_ERR("STUDIO", "Streamed card refresh failed: %s", path.c_str());
  return ok ? StreamResult::Shown : StreamResult::Failed;
}
void StudioFrame::displayed() {
  Writer lock(*this);
  if (alertFrame >= 0) {
    alertDisplayed = true;
    return;
  }
  if (active.size == BYTES || selectedFrame >= 0) active.displayed = true;
}
void StudioFrame::clear() {
  Writer lock(*this);
  abort(true);
  active = {};
  alertFrame = -1;
  alertDisplayed = false;
  savedProgram = {};
  lastVisual = {};
  lastVisualOffset = 0;
  program = {};
  playback = {};
  lastTickNow = 0;
  pixelOffset = 0;
  selectedFrame = -1;
  Storage.remove(STATE);
  Storage.remove(BACKUP);
  for (const auto& file : Storage.listFiles(ROOT, 1000)) {
    const std::string name = file.c_str();
    const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
    if ((ext == ".bin" || ext == ".ssp") && name.find("frames.bin") == std::string::npos) {
      const std::string path = name[0] == '/' ? name : std::string(ROOT) + "/" + name;
      Storage.remove(path.c_str());
    }
  }
  // The frames stay on the card (the area is never deleted) but nothing
  // references them any more.
  if (frame_store::areaReady()) frame_store::resetIndex();
  ++revision;
}
bool StudioFrame::hasContent() const { return hasContentView.load(); }
StudioFrame::Snapshot StudioFrame::snapshot() const {
  std::lock_guard<std::mutex> view(viewMutex);
  return activeView;
}
StudioFrame::Snapshot StudioFrame::displaySnapshot() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (alertFrame < 0) return active;
  auto visual = savedProgram;
  visual.card = program.cardIds[alertFrame];
  visual.displayed = alertDisplayed;
  return visual;
}
bool StudioFrame::busy() const { return receivingView.load(); }
size_t StudioFrame::received() const { return offsetView.load(); }
std::vector<StudioFrame::Kept> StudioFrame::keptFiles() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  std::vector<Kept> out;
  out.reserve(3);
  for (const Snapshot* s : {&active, &savedProgram, &lastVisual}) {
    if (s->hash.empty()) continue;
    bool seen = false;
    for (const auto& k : out) seen = seen || k.hash == s->hash;
    if (!seen) out.push_back({s->hash, s->size});
  }
  return out;
}

bool StudioFrame::readProgram(const Snapshot& source, studio::Program& result, size_t& start) {
  std::string path;
  size_t at = 0;
  if (!frame_store::headerOf(source.hash, source.size, path, at)) return false;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) return false;
  uint8_t prefix[8];
  // A whole file (header at 0) must be the content's size; a record was checked.
  if ((at == 0 && file.size() != source.size) || !file.seek(at) || file.read(prefix, 8) != 8 ||
      memcmp(prefix, "SSP1", 4) != 0) {
    file.close();
    return false;
  }
  const uint32_t length =
      uint32_t(prefix[4]) | uint32_t(prefix[5]) << 8 | uint32_t(prefix[6]) << 16 | uint32_t(prefix[7]) << 24;
  if (length > 32768 || length + 8 >= source.size) {
    file.close();
    return false;
  }
  std::string json(length, '\0');
  if (file.read(&json[0], length) != int(length)) {
    file.close();
    return false;
  }
  file.close();
  JsonDocument doc;
  if (deserializeJson(doc, json) || doc["version"] != 1) return false;
  auto frames = doc["frames"].as<JsonArray>();
  auto scenes = doc["scenes"].as<JsonArray>();
  auto plan = doc["plan"];
  if (frames.size() == 0 || 8 + length + frames.size() * BYTES != source.size || scenes.size() == 0 ||
      plan["windows"].size() > 64)
    return false;
  result.interval = plan["intervalSeconds"] | 0;
  if (result.interval < 15 || result.interval > 86400) return false;
  result.defaultScene = plan["defaultSceneId"] | "";
  result.alertScene = plan["alertSceneId"] | "";
  result.mode = plan["mode"] | "desktop";
  result.keyguard = plan["keyguardSeconds"] | 20;
  result.random = std::string(plan["rotation"] | "") == "random";
  result.alertInterrupts = plan["alertInterruptsOverride"] | false;
  result.calendarFrom = plan["calendar"]["from"] | "";
  result.calendarUntil = plan["calendar"]["until"] | "";
  for (auto day : plan["calendar"]["tradingDays"].as<JsonArray>())
    result.tradingDays.emplace_back(day.as<const char*>() ? day.as<const char*>() : "");
  for (auto frame : frames) {
    std::string id = frame["id"] | "";
    if (id.size() != 36 || std::find(result.cardIds.begin(), result.cardIds.end(), id) != result.cardIds.end())
      return false;
    result.cardIds.push_back(id);
  }
  std::vector<bool> used(frames.size(), false);
  for (auto row : scenes) {
    studio::Scene scene;
    scene.id = row["id"] | "";
    if (scene.id.size() != 36) return false;
    for (auto n : row["cards"].as<JsonArray>()) {
      if (!n.is<int>()) return false;
      int index = n.as<int>();
      if (index < 0 || size_t(index) >= frames.size() || used[index]) return false;
      used[index] = true;
      scene.cards.push_back(index);
    }
    if (scene.cards.empty()) return false;
    result.scenes.push_back(std::move(scene));
  }
  for (bool value : used)
    if (!value) return false;
  auto known = [&](const std::string& id) {
    return std::any_of(result.scenes.begin(), result.scenes.end(),
                       [&](const studio::Scene& scene) { return scene.id == id; });
  };
  if ((!result.defaultScene.empty() && !known(result.defaultScene)) ||
      (!result.alertScene.empty() && !known(result.alertScene)))
    return false;
  for (auto row : plan["windows"].as<JsonArray>()) {
    studio::Window window;
    window.id = row["id"] | "";
    window.scene = row["sceneId"] | "";
    window.start = row["start"] | "";
    window.end = row["end"] | "";
    window.repeat = row["repeat"] | "";
    window.priority = row["priority"] | 0;
    window.enabled = row["enabled"] | false;
    if (!known(window.scene) || studio::minute(window.start) < 0 || studio::minute(window.end) < 0 ||
        window.priority < 0 || window.priority > 99)
      return false;
    for (auto day : row["weekdays"].as<JsonArray>()) window.weekdays.push_back(day.as<int>());
    for (auto day : row["dates"].as<JsonArray>())
      window.dates.emplace_back(day.as<const char*>() ? day.as<const char*>() : "");
    result.windows.push_back(std::move(window));
  }
  start = 8 + length;
  return true;
}
void StudioFrame::restore() {
  Writer lock(*this);
  if (active.hash == savedProgram.hash) return;
  lastTickNow = 0;  // re-evaluate the restored program on the next tick
  if (savedProgram.hash.empty()) {
    if (!lastVisual.hash.empty() && lastVisual.size == BYTES) active = lastVisual;
    active.expires = 0;
    active.displayed = false;
    pixelOffset = 0;
    persist();
    ++revision;
    return;
  }
  active = savedProgram;
  alertFrame = -1;
  alertDisplayed = false;
  active.displayed = false;
  selectedFrame = -1;
  pixelOffset = headerOffset;
  persist();
  ++revision;
}
bool StudioFrame::hasProgram() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return !savedProgram.hash.empty();
}
void StudioFrame::tick(int64_t now, int event, int64_t alertUntil) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (now <= 0) return;  // no trusted time: hold the current frame
  // The UI loop calls this many times a second; schedules have one-second
  // resolution, so re-evaluate only when the second, the alert or a key changes.
  if (!event && now == lastTickNow && alertUntil == lastTickAlert) return;
  lastTickNow = now;
  lastTickAlert = alertUntil;
  if (active.expires > 0 && now >= active.expires) restore();
  if (active.hash.empty()) return;
  if (active.size == BYTES) {
    int overlay = -1;
    if (program.alertInterrupts && alertUntil > now && !program.alertScene.empty()) {
      alertScratch = playback;  // assignment reuses the scratch buffers
      overlay = studio::step(program, alertScratch, now, 0, alertUntil);
    }
    if (overlay != alertFrame) {
      alertFrame = overlay;
      alertDisplayed = false;
      active.displayed = false;
      publishLocked();
      ++revision;
    }
    if (!event) return;
    playback.lastKey = now;
    // "Next" on a temporary card returns to the installed program.
    if (alertFrame >= 0 || event != 1 || savedProgram.hash.empty()) return;
    restore();
  }
  if (program.cardIds.empty()) return;
  // The device-wide Nokia keyguard owns locking since 2.2.0, so every key that
  // reaches Studio acts: refresh lastKey first so the program's own
  // portable-mode first-press unlock never swallows it.
  if (event) playback.lastKey = now;
  const int index = studio::step(program, playback, now, event, alertUntil);
  if (index < 0) return;
  if (index != selectedFrame) {
    selectedFrame = index;
    pixelOffset = headerOffset + size_t(index) * BYTES;
    active.card = program.cardIds[index];
    active.displayed = false;
    publishLocked();
    persist();
    ++revision;
  } else if (event)
    persist();
}

int64_t StudioFrame::nextBoundary(int64_t now) const {
  // Asked from BLE host callbacks: while another task holds the lock for card
  // work, the last computed boundary answers (the plan changes only at commit).
  std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
  if (!lock.owns_lock()) return boundaryView.load();
  const int64_t boundary = now > 1735689600 && !savedProgram.hash.empty() ? studio::boundary(program, now) : -1;
  boundaryView.store(boundary);
  return boundary;
}

bool StudioFrame::portable() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return !savedProgram.hash.empty() && program.mode == "portable";
}

// Called without the lock: only the keep list is read under it. A file the
// UI loop starts to show meanwhile is in that list (active, saved, visual).
void StudioFrame::collectGarbage() {
  std::vector<std::string> keep;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    keep = keepListLocked();
  }
  for (const auto& file : Storage.listFiles(ROOT, 1000)) {
    std::string name = file.c_str();
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    // Whole files and records alike; a record's frames stay in their slots
    // (unpinned, so evicted oldest first when a transfer needs room).
    if (name.size() == 68 && (name.substr(64) == ".bin" || name.substr(64) == ".ssp") &&
        std::find(keep.begin(), keep.end(), name.substr(0, 64)) == keep.end()) {
      const auto path = std::string(ROOT) + "/" + name;
      Storage.remove(path.c_str());
    }
  }
}
