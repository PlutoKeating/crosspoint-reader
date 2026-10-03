#pragma once
#include <HalStorage.h>
#include <StudioProgram.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <mutex>
#include <string>

class GfxRenderer;

// A bounded stream of a frame or SSP1 program delivered over BLE (the only
// content channel). Rendering never reflows the content.
class StudioFrame {
 public:
  static constexpr size_t WIDTH = 528, HEIGHT = 792, BYTES = WIDTH * HEIGHT / 8, MAX_BYTES = 8000000;
  struct Snapshot {
    std::string task, hash;
    int64_t expires = 0;
    bool displayed = false;
    size_t size = BYTES;
    std::string card;
  };
  static StudioFrame& instance();
  void load();
  bool start(const std::string& task, const std::string& hash, int64_t expires, size_t size = BYTES);
  bool append(size_t offset, const uint8_t* data, size_t length);
  bool commit();
  void abort(bool discard = false);
  bool render(const GfxRenderer& renderer);
  void displayed();
  void clear();
  void tick(int64_t now, int event = 0, int64_t alertUntil = 0);
  void restore();
  bool hasProgram() const;
  bool portable() const;
  int64_t nextBoundary(int64_t now) const;
  Snapshot snapshot() const;
  // Allocation-free check for the UI loop (snapshot() copies heap strings).
  bool hasContent() const;
  Snapshot displaySnapshot() const;
  bool busy() const;
  size_t received() const;
  // Bytes of this exact transfer already on the SD card, from the RAM copy of
  // the partial-transfer record, so a BLE callback can answer `begin` without
  // touching the card. start() recomputes it from the file.
  size_t resumeOffset(const std::string& task, const std::string& hash, size_t size) const;
  uint32_t generation() const { return revision.load(); }

 private:
  mutable std::recursive_mutex mutex;
  Snapshot active, incoming, savedProgram, lastVisual;
  size_t lastVisualOffset = 0;
  studio::Program program;
  studio::Playback playback, alertScratch;
  int64_t lastTickNow = 0, lastTickAlert = 0;
  size_t pixelOffset = 0, headerOffset = 0;
  int selectedFrame = -1, alertFrame = -1;
  bool alertDisplayed = false;
  bool readProgram(const Snapshot& snapshot, studio::Program& result, size_t& start);
  HalFile output;
  mbedtls_sha256_context sha{};
  size_t offset = 0;
  // The partial transfer on the card (incoming.json + incoming.bin), mirrored
  // in RAM; empty task when there is none.
  struct Partial {
    std::string task, hash;
    size_t size = 0, bytes = 0;
  } partial;
  bool receiving = false, loaded = false;
  std::atomic<uint32_t> revision{0};
  bool persist();
  void collectGarbage();
};
