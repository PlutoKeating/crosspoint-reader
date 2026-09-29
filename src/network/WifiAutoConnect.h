#pragma once

#include <cstddef>
#include <cstdint>

// Non-blocking reconnection to saved Wi-Fi networks for the StockStick
// surface, so the device comes back online by itself after boot, deep-sleep
// wake, OTA restart or a dropped link. Tries the last connected network first,
// then the others in turn, with exponential backoff between rounds.
class WifiAutoConnect {
 public:
  // Call from the owning activity's loop. Returns true on the tick where the
  // link comes up (offline -> online edge).
  bool tick(uint32_t nowMs);
  // Retry immediately on the next tick (e.g. after the user closes Wi-Fi setup).
  void retrySoon() { nextAttemptMs = 0; }

  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
  static constexpr uint32_t FIRST_BACKOFF_MS = 10000;
  static constexpr uint32_t MAX_BACKOFF_MS = 5UL * 60UL * 1000UL;

 private:
  void startAttempt(uint32_t nowMs);

  bool loaded = false;
  bool connecting = false;
  bool wasConnected = false;
  uint8_t failures = 0;
  size_t candidate = 0;
  uint32_t attemptStartedMs = 0;
  uint32_t nextAttemptMs = 0;
};
