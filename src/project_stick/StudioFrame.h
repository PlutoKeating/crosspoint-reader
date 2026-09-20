#pragma once
#include <HalStorage.h>
#include <mbedtls/sha256.h>
#include <atomic>
#include <mutex>
#include <string>

class GfxRenderer;

// A bounded stream, shared by cloud and BLE. Rendering never reflows the content.
class StudioFrame {
 public:
  static constexpr size_t WIDTH = 528, HEIGHT = 792, BYTES = WIDTH * HEIGHT / 8;
  struct Snapshot { std::string task, hash, origin; int64_t expires = 0; bool displayed = false; };
  static StudioFrame& instance();
  void load();
  bool start(const std::string& task, const std::string& hash, int64_t expires, const std::string& origin);
  bool append(size_t offset, const uint8_t* data, size_t length);
  bool commit();
  void abort();
  bool render(const GfxRenderer& renderer);
  void displayed();
  void acknowledge(const std::string& task);
  bool reconciled(const std::string& task);
  void clear();
  Snapshot snapshot() const;
  bool needsReport() const;
  bool busy() const;
  size_t received() const;
  uint32_t generation() const { return revision.load(); }

 private:
  mutable std::recursive_mutex mutex;
  Snapshot active, incoming;
  HalFile output;
  mbedtls_sha256_context sha{};
  size_t offset = 0;
  bool receiving = false, reportPending = false, loaded = false;
  std::atomic<uint32_t> revision{0};
  bool persist();
};
