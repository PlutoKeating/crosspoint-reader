#include "StudioFrame.h"
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {
constexpr const char* ROOT = "/.crosspoint/studio";
constexpr const char* TEMP = "/.crosspoint/studio/incoming.bin";
constexpr const char* STATE = "/.crosspoint/studio/state.json";
constexpr const char* BACKUP = "/.crosspoint/studio/state.bak";
constexpr const char* STATE_TEMP = "/.crosspoint/studio/state.tmp";
std::string fileFor(const std::string& hash) { return std::string(ROOT) + "/" + hash + ".bin"; }
bool validHash(const std::string& hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool verifyFile(const std::string& path, const std::string& expected) {
  HalFile file; if (!Storage.openFileForRead("STUDIO", path, file)) return false;
  if (file.size() != StudioFrame::BYTES) { file.close(); return false; }
  mbedtls_sha256_context digest; mbedtls_sha256_init(&digest); mbedtls_sha256_starts(&digest, 0);
  uint8_t block[512], sum[32]; size_t remaining = StudioFrame::BYTES;
  bool valid = true;
  while (remaining) {
    const size_t count = std::min(remaining, sizeof(block));
    if (file.read(block, count) != count) { valid = false; break; }
    mbedtls_sha256_update(&digest, block, count); remaining -= count;
  }
  file.close(); mbedtls_sha256_finish(&digest, sum); mbedtls_sha256_free(&digest);
  char text[65]; for (size_t i = 0; i < 32; ++i) snprintf(text + i * 2, 3, "%02x", sum[i]);
  return valid && expected == text;
}
}

StudioFrame& StudioFrame::instance() { static StudioFrame frame; return frame; }
void StudioFrame::load() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (loaded) return;
  loaded = true;
  Storage.mkdir(ROOT, true);
  for (const char* path : {STATE, BACKUP}) {
    HalFile input;
    if (!Storage.openFileForRead("STUDIO", path, input)) continue;
    JsonDocument doc;
    const auto error = deserializeJson(doc, input); input.close();
    if (error) continue;
    const std::string hash = doc["hash"] | "";
    if (!validHash(hash)) continue;
    if (!verifyFile(fileFor(hash), hash)) continue;
    active = {doc["task"] | "", hash, doc["origin"] | "cloud", doc["expires"] | int64_t(0), false};
    ++revision;
    break;
  }
}
bool StudioFrame::start(const std::string& task, const std::string& hash, int64_t expires, const std::string& origin) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (receiving || task.size() != 36 || !validHash(hash)) return false;
  Storage.mkdir(ROOT, true); Storage.remove(TEMP);
  if (!Storage.openFileForWrite("STUDIO", TEMP, output)) return false;
  incoming = {task, hash, origin, expires, false}; offset = 0;
  mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0); receiving = true;
  return true;
}
bool StudioFrame::append(size_t expectedOffset, const uint8_t* data, size_t length) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || expectedOffset != offset || length > BYTES - offset || !length) return false;
  if (output.write(data, length) != length) { abort(); return false; }
  mbedtls_sha256_update(&sha, data, length); offset += length; return true;
}
bool StudioFrame::commit() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (!receiving || offset != BYTES) { abort(); return false; }
  uint8_t bytes[32]; mbedtls_sha256_finish(&sha, bytes); mbedtls_sha256_free(&sha);
  receiving = false; output.close();
  char digest[65]; for (size_t i = 0; i < 32; ++i) snprintf(digest + i * 2, 3, "%02x", bytes[i]);
  if (incoming.hash != digest) { Storage.remove(TEMP); return false; }
  const std::string path = fileFor(incoming.hash);
  if (Storage.exists(path.c_str()) && verifyFile(path, incoming.hash)) Storage.remove(TEMP);
  else { Storage.remove(path.c_str()); if (!Storage.rename(TEMP, path.c_str())) return false; }
  const Snapshot old = active; active = incoming;
  if (!persist()) { active = old; return false; }
  reportPending = false; ++revision;
  return true;
}
void StudioFrame::abort() {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (receiving) { output.close(); mbedtls_sha256_free(&sha); receiving = false; }
  Storage.remove(TEMP); offset = 0;
}
bool StudioFrame::persist() {
  JsonDocument doc; doc["task"] = active.task; doc["hash"] = active.hash;
  doc["origin"] = active.origin; doc["expires"] = active.expires;
  Storage.remove(STATE_TEMP);
  HalFile file; if (!Storage.openFileForWrite("STUDIO", STATE_TEMP, file)) return false;
  const bool written = serializeJson(doc, file) > 0; file.close();
  if (!written) return false;
  Storage.remove(BACKUP);
  if (Storage.exists(STATE) && !Storage.rename(STATE, BACKUP)) return false;
  if (!Storage.rename(STATE_TEMP, STATE)) { if (Storage.exists(BACKUP)) Storage.rename(BACKUP, STATE); return false; }
  return true;
}
bool StudioFrame::render(const GfxRenderer& renderer) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (active.hash.empty() || renderer.getScreenWidth() != WIDTH || renderer.getScreenHeight() != HEIGHT) return false;
  HalFile file; if (!Storage.openFileForRead("STUDIO", fileFor(active.hash), file)) return false;
  if (file.size() != BYTES) { file.close(); return false; }
  renderer.clearScreen(); uint8_t row[WIDTH / 8];
  for (size_t y = 0; y < HEIGHT; ++y) {
    if (file.read(row, sizeof(row)) != sizeof(row)) { file.close(); return false; }
    for (size_t x = 0; x < WIDTH; ++x) if (row[x / 8] & (0x80 >> (x & 7))) renderer.drawPixel(x, y, true);
  }
  file.close(); return true;
}
void StudioFrame::displayed() { std::lock_guard<std::recursive_mutex> lock(mutex); if (!active.displayed) { active.displayed = true; reportPending = true; } }
void StudioFrame::acknowledge(const std::string& task) { std::lock_guard<std::recursive_mutex> lock(mutex); if (active.task == task) reportPending = false; }
bool StudioFrame::reconciled(const std::string& task) {
  std::lock_guard<std::recursive_mutex> lock(mutex);
  if (active.task != task || active.origin != "ble") return false;
  active.origin = "cloud";
  if (!persist()) { active.origin = "ble"; return false; }
  reportPending = false; return true;
}
void StudioFrame::clear() { std::lock_guard<std::recursive_mutex> lock(mutex); if (active.hash.empty()) return; active = {}; reportPending = false; Storage.remove(STATE); Storage.remove(BACKUP); ++revision; }
StudioFrame::Snapshot StudioFrame::snapshot() const { std::lock_guard<std::recursive_mutex> lock(mutex); return active; }
bool StudioFrame::needsReport() const { std::lock_guard<std::recursive_mutex> lock(mutex); return reportPending; }
bool StudioFrame::busy() const { std::lock_guard<std::recursive_mutex> lock(mutex); return receiving; }
size_t StudioFrame::received() const { std::lock_guard<std::recursive_mutex> lock(mutex); return offset; }
