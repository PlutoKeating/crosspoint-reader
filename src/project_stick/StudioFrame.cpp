#include "StudioFrame.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {
constexpr const char* ROOT = "/.crosspoint/studio";
constexpr const char* TEMP = "/.crosspoint/studio/incoming.bin";
constexpr const char* PARTIAL = "/.crosspoint/studio/incoming.json";
constexpr const char* STATE = "/.crosspoint/studio/state.json";
constexpr const char* BACKUP = "/.crosspoint/studio/state.bak";
constexpr const char* STATE_TEMP = "/.crosspoint/studio/state.tmp";
std::string fileFor(const std::string& hash) { return std::string(ROOT) + "/" + hash + ".bin"; }
bool validHash(const std::string& hash) {
  return hash.size() == 64 &&
         std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool verifyFile(const std::string& path, const std::string& expected) {
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", path, file)) return false;
  if (file.size() < StudioFrame::BYTES || file.size() > StudioFrame::MAX_BYTES) {
    file.close();
    return false;
  }
  mbedtls_sha256_context digest;
  mbedtls_sha256_init(&digest);
  mbedtls_sha256_starts(&digest, 0);
  uint8_t block[512], sum[32];
  size_t remaining = file.size();
  bool valid = true;
  while (remaining) {
    const size_t count = std::min(remaining, sizeof(block));
    if (file.read(block, count) != count) {
      valid = false;
      break;
    }
    mbedtls_sha256_update(&digest, block, count);
    remaining -= count;
  }
  file.close();
  mbedtls_sha256_finish(&digest, sum);
  mbedtls_sha256_free(&digest);
  char text[65];
  for (size_t i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", sum[i]);
  return valid && expected == text;
}
}  // namespace

StudioFrame& StudioFrame::instance() {
  static StudioFrame frame;
  return frame;
}
void StudioFrame::load() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (loaded) return;
  loaded = true;
  Storage.mkdir(ROOT, true);
  for (const char* path : {STATE, BACKUP}) {
    HalFile input;
    if (!Storage.openFileForRead("STUDIO", path, input)) continue;
    JsonDocument doc;
    const auto error = deserializeJson(doc, input);
    input.close();
    if (error) continue;
    const std::string hash = doc["hash"] | "";
    if (!validHash(hash)) continue;
    if (!verifyFile(fileFor(hash), hash)) continue;
    active = {doc["task"] | "", hash, doc["origin"] | "cloud", doc["expires"] | int64_t(0), false, doc["size"] | BYTES,
              doc["card"] | ""};
    auto visual = doc["visual"];
    lastVisual = {visual["task"] | "", visual["hash"] | "", "cloud", 0, false, visual["size"] | BYTES, ""};
    lastVisualOffset = visual["offset"] | size_t(0);
    if (!validHash(lastVisual.hash) || !verifyFile(fileFor(lastVisual.hash), lastVisual.hash) ||
        lastVisualOffset + BYTES > lastVisual.size)
      lastVisual = {};
    auto saved = doc["program"];
    savedProgram = {
        saved["task"] | "", saved["hash"] | "", saved["origin"] | "cloud", 0, false, saved["size"] | BYTES, ""};
    if (active.size > BYTES) savedProgram = active;
    if (!savedProgram.hash.empty() && validHash(savedProgram.hash) &&
        verifyFile(fileFor(savedProgram.hash), savedProgram.hash) && readProgram(savedProgram, program, headerOffset)) {
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
bool StudioFrame::start(const std::string& task, const std::string& hash, int64_t expires, const std::string& origin,
                        size_t size) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (receiving || task.size() != 36 || !validHash(hash) || size < BYTES || size > MAX_BYTES) return false;
  Storage.mkdir(ROOT, true);
  incoming = {task, hash, origin, expires, false, size, ""};
  offset = 0;
  bool resume = false;
  HalFile metadata;
  if (Storage.openFileForRead("STUDIO", PARTIAL, metadata)) {
    JsonDocument doc;
    const bool valid = !deserializeJson(doc, metadata);
    metadata.close();
    resume = valid && std::string(doc["task"] | "") == task && std::string(doc["hash"] | "") == hash &&
             (doc["size"] | size_t(0)) == size;
  }
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  if (resume) {
    HalFile file;
    if (Storage.openFileForRead("STUDIO", TEMP, file) && file.size() <= size) {
      uint8_t chunk[512];
      while (file.available()) {
        const int count = file.read(chunk, sizeof(chunk));
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
  }
  const uint64_t total = Storage.totalBytes(), used = Storage.usedBytes();
  if (total <= used || total - used < size - offset + 65536) {
    mbedtls_sha256_free(&sha);
    return false;
  }
  output = Storage.open(TEMP, O_WRONLY | O_CREAT | O_APPEND);
  if (!output) {
    mbedtls_sha256_free(&sha);
    return false;
  }
  JsonDocument doc;
  doc["task"] = task;
  doc["hash"] = hash;
  doc["size"] = size;
  HalFile meta;
  if (!Storage.openFileForWrite("STUDIO", PARTIAL, meta)) {
    output.close();
    mbedtls_sha256_free(&sha);
    return false;
  }
  const bool saved = serializeJson(doc, meta) > 0;
  meta.close();
  if (!saved) {
    output.close();
    mbedtls_sha256_free(&sha);
    return false;
  }
  receiving = true;
  return true;
}
bool StudioFrame::append(size_t expectedOffset, const uint8_t* data, size_t length) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || expectedOffset != offset || length > incoming.size - offset || !length) return false;
  if (output.write(data, length) != length) {
    abort();
    return false;
  }
  mbedtls_sha256_update(&sha, data, length);
  offset += length;
  return true;
}
bool StudioFrame::commit() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || offset != incoming.size) {
    abort();
    return false;
  }
  uint8_t bytes[32];
  mbedtls_sha256_finish(&sha, bytes);
  mbedtls_sha256_free(&sha);
  receiving = false;
  output.close();
  char digest[65];
  for (size_t i = 0; i < 32; ++i) snprintf(digest + i * 2, 3, "%02x", bytes[i]);
  if (incoming.hash != digest) {
    Storage.remove(TEMP);
    Storage.remove(PARTIAL);
    return false;
  }
  Storage.remove(PARTIAL);
  const std::string path = fileFor(incoming.hash);
  if (Storage.exists(path.c_str()) && verifyFile(path, incoming.hash))
    Storage.remove(TEMP);
  else {
    Storage.remove(path.c_str());
    if (!Storage.rename(TEMP, path.c_str())) return false;
  }
  studio::Program nextProgram;
  size_t nextHeader = 0;
  if (incoming.size > BYTES && !readProgram(incoming, nextProgram, nextHeader)) {
    Storage.remove(path.c_str());
    return false;
  }
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
  active = incoming;
  alertFrame = -1;
  alertDisplayed = false;
  pixelOffset = 0;
  feedbackSignal = 0;
  if (incoming.size > BYTES) {
    savedProgram = incoming;
    program = std::move(nextProgram);
    headerOffset = nextHeader;
    playback = {};
    selectedFrame = -1;
    pixelOffset = headerOffset;
  }
  if (!persist()) {
    active = old;
    savedProgram = oldSaved;
    program = oldProgram;
    playback = oldPlayback;
    pixelOffset = oldOffset;
    headerOffset = oldHeader;
    lastVisual = oldVisual;
    lastVisualOffset = oldVisualOffset;
    selectedFrame = oldSelected;
    alertFrame = oldAlert;
    alertDisplayed = oldAlertDisplayed;
    return false;
  }
  collectGarbage();
  reportPending = false;
  ++revision;
  return true;
}
void StudioFrame::abort(bool discard) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (receiving) {
    output.close();
    mbedtls_sha256_free(&sha);
    receiving = false;
  }
  if (discard) {
    Storage.remove(TEMP);
    Storage.remove(PARTIAL);
    offset = 0;
  }
}
bool StudioFrame::persist() {
  JsonDocument doc;
  doc["task"] = active.task;
  doc["hash"] = active.hash;
  doc["origin"] = active.origin;
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
  saved["origin"] = savedProgram.origin;
  saved["size"] = savedProgram.size;
  auto state = doc["playback"].to<JsonObject>();
  state["time"] = playback.lastTime;
  state["change"] = playback.lastChange;
  state["until"] = playback.manualUntil;
  state["scene"] = playback.scene;
  state["card"] = playback.lastCard;
  state["index"] = playback.index;
  state["manual"] = playback.manual;
  Storage.remove(STATE_TEMP);
  HalFile file;
  if (!Storage.openFileForWrite("STUDIO", STATE_TEMP, file)) return false;
  const bool written = serializeJson(doc, file) > 0;
  file.close();
  if (!written) return false;
  Storage.remove(BACKUP);
  if (Storage.exists(STATE) && !Storage.rename(STATE, BACKUP)) return false;
  if (!Storage.rename(STATE_TEMP, STATE)) {
    if (Storage.exists(BACKUP)) Storage.rename(BACKUP, STATE);
    return false;
  }
  return true;
}
bool StudioFrame::render(const GfxRenderer& renderer) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (active.hash.empty() || renderer.getScreenWidth() != WIDTH || renderer.getScreenHeight() != HEIGHT) return false;
  const bool hold = active.size > BYTES && selectedFrame < 0;
  const auto& visual = alertFrame >= 0 ? savedProgram : hold ? lastVisual : active;
  const size_t start = alertFrame >= 0 ? headerOffset + size_t(alertFrame) * BYTES
                       : hold          ? lastVisualOffset
                                       : pixelOffset;
  if (visual.hash.empty()) return false;
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", fileFor(visual.hash), file)) return false;
  if (file.size() != visual.size || !file.seek(start)) {
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
  if (feedbackSignal > 0 && feedbackSignal <= 3) {
    static const uint8_t glyphs[3][8] = {{0x1c, 0x22, 0x20, 0x7e, 0x42, 0x5a, 0x42, 0x7e},
                                         {0x08, 0x0c, 0xfe, 0xff, 0xfe, 0x0c, 0x08, 0},
                                         {0, 0x01, 0x03, 0x86, 0xcc, 0x78, 0x30, 0}};
    for (int y = 0; y < 40; y++)
      for (int x = 0; x < 56; x++) {
        const int gx = (x - 16) / 3, gy = (y - 8) / 3;
        const bool ink = x == 0 || x == 55 || y == 0 || y == 39 ||
                         (x >= 16 && y >= 8 && gx < 8 && gy < 8 && (glyphs[feedbackSignal - 1][gy] & (0x80 >> gx)));
        renderer.drawPixel(x + 236, y + 732, ink);
      }
  }
  return true;
}
void StudioFrame::displayed() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (alertFrame >= 0) {
    if (!alertDisplayed && feedbackSignal == 0) {
      alertDisplayed = true;
      reportPending = true;
    }
    return;
  }
  if (!active.displayed && feedbackSignal == 0 && (active.size == BYTES || selectedFrame >= 0)) {
    active.displayed = true;
    reportPending = true;
  }
}
void StudioFrame::acknowledge(const std::string& task) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if ((alertFrame >= 0 ? savedProgram.task : active.task) == task) reportPending = false;
}
bool StudioFrame::reconciled(const std::string& task) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (active.task != task || active.origin != "ble") return false;
  active.origin = "cloud";
  if (savedProgram.task == task) savedProgram.origin = "cloud";
  if (!persist()) {
    active.origin = "ble";
    return false;
  }
  reportPending = active.displayed;
  return true;
}
void StudioFrame::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  abort(true);
  active = {};
  alertFrame = -1;
  alertDisplayed = false;
  savedProgram = {};
  lastVisual = {};
  lastVisualOffset = 0;
  program = {};
  playback = {};
  pixelOffset = 0;
  selectedFrame = -1;
  reportPending = false;
  Storage.remove(STATE);
  Storage.remove(BACKUP);
  for (const auto& file : Storage.listFiles(ROOT, 1000)) {
    const std::string name = file.c_str();
    if (name.size() > 4 && name.substr(name.size() - 4) == ".bin") {
      const std::string path = name[0] == '/' ? name : std::string(ROOT) + "/" + name;
      Storage.remove(path.c_str());
    }
  }
  ++revision;
}
StudioFrame::Snapshot StudioFrame::snapshot() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return active;
}
StudioFrame::Snapshot StudioFrame::displaySnapshot() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (alertFrame < 0) return active;
  auto visual = savedProgram;
  visual.card = program.cardIds[alertFrame];
  visual.displayed = alertDisplayed;
  return visual;
}
bool StudioFrame::needsReport() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return reportPending;
}
bool StudioFrame::busy() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return receiving;
}
size_t StudioFrame::received() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return offset;
}

bool StudioFrame::readProgram(const Snapshot& source, studio::Program& result, size_t& start) {
  HalFile file;
  if (!Storage.openFileForRead("STUDIO", fileFor(source.hash), file)) return false;
  uint8_t prefix[8];
  if (file.size() != source.size || file.read(prefix, 8) != 8 || memcmp(prefix, "SSP1", 4) != 0) {
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
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (active.hash == savedProgram.hash) return;
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
  feedbackSignal = 0;
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
  if (now <= 0) return;
  if (active.expires > 0 && now >= active.expires) restore();
  if (feedbackSignal && millis() - feedbackAt > 2000) {
    feedbackSignal = 0;
    ++revision;
  }
  if (active.hash.empty()) return;
  if (active.size == BYTES) {
    int overlay = -1;
    if (program.alertInterrupts && alertUntil > now && !program.alertScene.empty()) {
      auto state = playback;
      overlay = studio::step(program, state, now, 0, alertUntil);
    }
    if (overlay != alertFrame) {
      alertFrame = overlay;
      alertDisplayed = false;
      active.displayed = false;
      reportPending = false;
      ++revision;
    }
    if (!event) return;
    const bool locked = program.mode == "portable" && program.keyguard > 0 && now - playback.lastKey > program.keyguard;
    playback.lastKey = now;
    feedbackSignal = locked ? 1 : event == 1 ? 2 : 3;
    feedbackAt = millis();
    ++revision;
    if (locked || alertFrame >= 0 || event != 1 || savedProgram.hash.empty()) return;
    restore();
  }
  if (program.cardIds.empty()) return;
  const int index = studio::step(program, playback, now, event, alertUntil);
  if (event && playback.signal) {
    feedbackSignal = playback.signal;
    feedbackAt = millis();
    ++revision;
  }
  if (index < 0) return;
  if (index != selectedFrame) {
    selectedFrame = index;
    pixelOffset = headerOffset + size_t(index) * BYTES;
    active.card = program.cardIds[index];
    active.displayed = false;
    persist();
    ++revision;
  } else if (event)
    persist();
}

int64_t StudioFrame::nextBoundary(int64_t now) const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return now > 1735689600 && !savedProgram.hash.empty() ? studio::boundary(program, now) : -1;
}

bool StudioFrame::guardKey(int64_t now) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  const bool locked = program.mode == "portable" && program.keyguard > 0 && now - playback.lastKey > program.keyguard;
  playback.lastKey = now;
  if (locked) {
    feedbackSignal = 1;
    feedbackAt = millis();
    ++revision;
  }
  return locked;
}

bool StudioFrame::portable() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return !savedProgram.hash.empty() && program.mode == "portable";
}

int StudioFrame::feedback() const {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  return feedbackSignal;
}

void StudioFrame::collectGarbage() {
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
  for (const auto& file : Storage.listFiles(ROOT, 1000)) {
    std::string name = file.c_str();
    const auto slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() == 68 && name.substr(64) == ".bin" &&
        std::find(keep.begin(), keep.end(), name.substr(0, 64)) == keep.end()) {
      const auto path = std::string(ROOT) + "/" + name;
      Storage.remove(path.c_str());
    }
  }
}
