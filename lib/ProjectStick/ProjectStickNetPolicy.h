#pragma once

#include <cstdint>

// Pure decisions behind the device's few HTTPS requests (docs/studio-protocol.md
// "Cloud requests"): whether the heap can afford a TLS handshake, why a request
// failed (shown to the user instead of a spinner), and UI deadlines.
namespace project_stick {

// What a wolfSSL TLS 1.3 handshake plus record buffers needs on the C3. The
// handshake is known to work at the ~45-50 KB free a busy session leaves when
// NimBLE is not initialised; below these marks the request is sent with the
// BLE stack released (deinitialised) to lend its heap.
constexpr uint32_t TLS_MIN_FREE_HEAP = 56 * 1024;
constexpr uint32_t TLS_MIN_MAX_ALLOC = 24 * 1024;

inline bool tlsHeapSufficient(uint32_t freeHeap, uint32_t maxAlloc) {
  return freeHeap >= TLS_MIN_FREE_HEAP && maxAlloc >= TLS_MIN_MAX_ALLOC;
}

// Hard floor, checked after NimBLE has been released: below it a handshake
// cannot complete (the 2.2.2 crash log shows three 40 s attempts failing at
// 23-26 KB free / 17-23 KB largest block) and each attempt only fragments the
// heap further. Such a request is skipped and reported as a memory failure.
constexpr uint32_t TLS_HARD_MIN_FREE_HEAP = 32 * 1024;
constexpr uint32_t TLS_HARD_MIN_MAX_ALLOC = 16 * 1024;

inline bool tlsHeapAffordable(uint32_t freeHeap, uint32_t maxAlloc) {
  return freeHeap >= TLS_HARD_MIN_FREE_HEAP && maxAlloc >= TLS_HARD_MIN_MAX_ALLOC;
}

enum class NetFailure : uint8_t {
  None,
  Clock,        // no trusted time (NTP unreachable): certificates cannot be checked
  Network,      // DNS / TCP / TLS / read failed or timed out
  Memory,       // transport failure while the heap was below the TLS marks
  RateLimited,  // 429, or the shared backoff is holding requests
  Server,       // any other HTTP error status
  Timeout,      // the UI deadline passed before the worker answered
};

// Classifies one request outcome. `status` is the HTTP status, <= 0 for a
// transport failure; `heapLow` is the heap state when the transport failed.
inline NetFailure classifyRequest(bool clockReady, int status, bool heapLow) {
  if (!clockReady) return NetFailure::Clock;
  if (status <= 0) return heapLow ? NetFailure::Memory : NetFailure::Network;
  if (status == 429) return NetFailure::RateLimited;
  if (status >= 200 && status < 300) return NetFailure::None;
  return NetFailure::Server;
}

// A one-shot deadline on the millis() clock (wrap-safe).
class Deadline {
 public:
  void start(uint32_t nowMs, uint32_t durationMs) {
    startedMs_ = nowMs;
    durationMs_ = durationMs;
    active_ = true;
  }
  void stop() { active_ = false; }
  bool active() const { return active_; }
  bool expired(uint32_t nowMs) const { return active_ && nowMs - startedMs_ >= durationMs_; }

 private:
  uint32_t startedMs_ = 0;
  uint32_t durationMs_ = 0;
  bool active_ = false;
};

// How long the UI waits for the worker: a firmware check (queueing behind a
// running job included), and a BLE-triggered install waiting for the worker.
constexpr uint32_t FIRMWARE_CHECK_DEADLINE_MS = 60000;
constexpr uint32_t OTA_QUEUE_DEADLINE_MS = 60000;

}  // namespace project_stick
