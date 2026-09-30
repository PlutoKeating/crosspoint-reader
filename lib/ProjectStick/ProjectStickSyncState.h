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

// Register heartbeat of a bound device (binding, owner, trading day, clock).
inline bool registrationDue(uint32_t nowMs, uint32_t lastSuccessMs, uint32_t intervalMs = 6UL * 60UL * 60UL * 1000UL) {
  return lastSuccessMs == 0 || nowMs - lastSuccessMs >= intervalMs;
}

}  // namespace project_stick
