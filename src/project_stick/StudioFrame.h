#pragma once
#include <HalDisplay.h>
#include <HalStorage.h>
#include <StudioProgram.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

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
  static std::string fileFor(const std::string& hash);
  void load();
  // Opens incoming.bin for `task`/`hash`. A matching partial resumes from
  // min(its length, resumeLimit): the BLE receiver (protocol 4) resumes only
  // at frame boundaries and overwrites whatever lies past the limit.
  bool start(const std::string& task, const std::string& hash, int64_t expires, size_t size = BYTES,
             size_t resumeLimit = MAX_BYTES);
  bool append(size_t offset, const uint8_t* data, size_t length);
  bool commit();
  void abort(bool discard = false);
  bool render(const GfxRenderer& renderer);
  // Shows the current visual straight from the SD card, one strip of panel
  // columns at a time, with `overlay` drawn over each strip (the renderer's
  // strip target is active while it runs; it must draw the same pixels every
  // call). No framebuffer is touched. Unavailable: nothing drawn (no visual,
  // a panel or orientation that cannot stream, no memory for a strip) — use
  // render(). Failed: the panel was refreshed but a read failed.
  enum class StreamResult : uint8_t { Shown, Unavailable, Failed };
  using Overlay = void (*)(const GfxRenderer& renderer, void* ctx);
  StreamResult stream(const GfxRenderer& renderer, Overlay overlay, void* ctx, HalDisplay::RefreshMode mode);
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
  // Card usage for the capacity check and the phone's metrics. The free-cluster
  // scan behind it reads the whole FAT (seconds on a large card), so only
  // refreshStorageUsage() scans — called from the background worker when no
  // transfer runs — and storageUsage() returns the last reading (false: none yet).
  static bool storageUsage(uint64_t& total, uint64_t& used);
  static void refreshStorageUsage();
  size_t received() const;
  // Makes appended bytes readable through a second handle (the receiver reads
  // the SSP1 header back to resolve its frames).
  bool flushOutput();
  // Program and frame files the device keeps (active, saved program, last
  // visual and the backup state's): the protocol 4 receiver copies frames it
  // already holds from these instead of receiving them again.
  struct Kept {
    std::string hash;
    size_t size = 0;
  };
  std::vector<Kept> keptFiles() const;
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
  // The file and offset of the frame render()/stream() show; false: none.
  bool visualSource(std::string& path, size_t& size, size_t& start) const;
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
