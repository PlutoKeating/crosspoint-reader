#pragma once
#include <HalDisplay.h>
#include <HalStorage.h>
#include <StudioProgram.h>
#include <mbedtls/sha256.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class GfxRenderer;
namespace frame_store {
class Incoming;
}

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
  // Where the received SSP1 header can be read back (the receiver resolves
  // its frames from it); flushes the writes first.
  bool incomingHeader(std::string& path, size_t& start);
  // Frame slots (2.7.10), while a transfer writes to them: the slot holding a
  // frame with this digest (0xffff: none), and referencing that slot as the
  // next frame instead of receiving or copying it.
  bool slotsActive() const;
  uint16_t findSlot(const std::array<uint8_t, 32>& digest) const;
  bool appendSlot(uint16_t slot);
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
  // `mutex` guards the state below and is held across card access (start,
  // append, persist). BLE host callbacks read only the published copies
  // (snapshot, busy, received, resumeOffset, nextBoundary), so a slow SD write
  // never stalls the NimBLE host task (2.7.10).
  mutable std::recursive_mutex mutex;
  class Writer;  // holds `mutex` and publishes the copies when released
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
  bool visualSource(std::string& path, size_t& start) const;
  HalFile output;  // incoming.bin (whole-file transfers)
  std::unique_ptr<frame_store::Incoming> slotsIn;  // a transfer into frame slots
  void closeIncomingLocked();
  // Content the card must keep (and whose frame slots stay pinned).
  std::vector<std::string> keepListLocked() const;
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
  mutable std::mutex viewMutex;  // never held across card access or `mutex`
  Snapshot activeView;
  std::string partialTaskView, partialHashView;
  size_t partialSizeView = 0;
  std::atomic<size_t> partialBytesView{0}, offsetView{0};
  std::atomic<bool> receivingView{false}, hasContentView{false};
  mutable std::atomic<int64_t> boundaryView{-1};
  void publishLocked();
  bool persist();
  void collectGarbage();
};
