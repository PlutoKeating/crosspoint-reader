#pragma once
#include <HalStorage.h>
#include <StudioProgram.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <mutex>
#include <string>

class GfxRenderer;

// A bounded stream, shared by cloud and BLE. Rendering never reflows the content.
class StudioFrame {
 public:
  static constexpr size_t WIDTH = 528, HEIGHT = 792, BYTES = WIDTH * HEIGHT / 8, MAX_BYTES = 8000000;
  struct Snapshot {
    std::string task, hash, origin;
    int64_t expires = 0;
    bool displayed = false;
    size_t size = BYTES;
    std::string card;
  };
  static StudioFrame& instance();
  void load();
  bool start(const std::string& task, const std::string& hash, int64_t expires, const std::string& origin,
             size_t size = BYTES);
  bool append(size_t offset, const uint8_t* data, size_t length);
  bool commit();
  void abort(bool discard = false);
  bool render(const GfxRenderer& renderer);
  void displayed();
  void acknowledge(const std::string& task);
  bool reconciled(const std::string& task);
  void clear();
  void tick(int64_t now, int event = 0, int64_t alertUntil = 0);
  void restore();
  bool hasProgram() const;
  bool portable() const;
  bool guardKey(int64_t now);
  int feedback() const;
  int64_t nextBoundary(int64_t now) const;
  Snapshot snapshot() const;
  Snapshot displaySnapshot() const;
  bool needsReport() const;
  bool busy() const;
  size_t received() const;
  uint32_t generation() const { return revision.load(); }

 private:
  mutable std::recursive_mutex mutex;
  Snapshot active, incoming, savedProgram, lastVisual;
  size_t lastVisualOffset = 0;
  studio::Program program;
  studio::Playback playback;
  size_t pixelOffset = 0, headerOffset = 0;
  int selectedFrame = -1, alertFrame = -1;
  bool alertDisplayed = false;
  int feedbackSignal = 0;
  uint32_t feedbackAt = 0;
  bool readProgram(const Snapshot& snapshot, studio::Program& result, size_t& start);
  HalFile output;
  mbedtls_sha256_context sha{};
  size_t offset = 0;
  bool receiving = false, reportPending = false, loaded = false;
  std::atomic<uint32_t> revision{0};
  bool persist();
  void collectGarbage();
};
