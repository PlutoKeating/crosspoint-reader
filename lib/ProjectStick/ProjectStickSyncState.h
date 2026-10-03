#pragma once

#include <cstdint>

#include "ProjectStickCore.h"

namespace project_stick {

enum class SyncResult { Synced, Unbound, Failed, Inactive };

struct SyncReport {
  SyncResult result = SyncResult::Failed;
  bool registerAttempted = false;
  bool registerSucceeded = false;
  ShanghaiTime synchronizedAt;
};

class BackgroundWorkGate {
 public:
  bool tryQueue() {
    if (pending_ || running_) return false;
    pending_ = true;
    return true;
  }

  bool begin() {
    if (!pending_ || running_) return false;
    pending_ = false;
    running_ = true;
    return true;
  }

  void complete() { running_ = false; }
  bool pending() const { return pending_; }
  bool running() const { return running_; }
  bool busy() const { return pending_ || running_; }

 private:
  bool pending_ = false;
  bool running_ = false;
};

inline bool shouldRecordRegisterSuccess(const SyncReport& report) {
  return report.registerAttempted && report.registerSucceeded;
}

constexpr uint32_t HEARTBEAT_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr int64_t HEARTBEAT_INTERVAL_SECONDS = 6LL * 60 * 60;

// Register heartbeat of a bound device (binding, owner, trading day, clock).
inline bool registrationDue(uint32_t nowMs, uint32_t lastSuccessMs, uint32_t intervalMs = HEARTBEAT_INTERVAL_MS) {
  return lastSuccessMs == 0 || nowMs - lastSuccessMs >= intervalMs;
}

// BLE-first (2.6.0): a phone that synced the device over Bluetooth relayed
// everything the heartbeat would carry, so the cloud heartbeat is skipped
// while that sync is younger than the heartbeat interval. `nowUtc` and
// `lastPhoneSyncUtc` are Unix seconds, 0 when unknown; a clock that runs
// backwards (RTC lost, server time adopted) counts as stale, not fresh.
inline bool phoneSyncFresh(int64_t nowUtc, int64_t lastPhoneSyncUtc, int64_t intervalSeconds = HEARTBEAT_INTERVAL_SECONDS) {
  if (nowUtc <= 0 || lastPhoneSyncUtc <= 0 || nowUtc < lastPhoneSyncUtc) return false;
  return nowUtc - lastPhoneSyncUtc < intervalSeconds;
}

}  // namespace project_stick
