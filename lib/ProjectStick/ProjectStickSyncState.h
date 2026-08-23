#pragma once

#include "ProjectStickCore.h"

#include <cstdint>

namespace project_stick {

enum class SyncResult { Updated, Unchanged, OfflineCache, NoContent, Failed, Inactive };

struct SyncReport {
  SyncResult result = SyncResult::Failed;
  bool registerAttempted = false;
  bool registerSucceeded = false;
  bool manifestAttempted = false;
  bool manifestCompleted = false;
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

inline bool shouldRecordManifestPoll(const SyncReport& report) {
  return report.manifestAttempted && report.manifestCompleted;
}

inline SyncResult contentSelectionFailureResult(uint32_t activeVersion) {
  return activeVersion == 0 ? SyncResult::NoContent : SyncResult::Failed;
}

inline bool scheduleCacheNeedsReload(uint32_t cachedVersion, uint32_t activeVersion,
                                     bool cacheEmpty) {
  return cacheEmpty || cachedVersion != activeVersion;
}

inline bool registrationDue(uint32_t nowMs, uint32_t lastSuccessMs,
                            uint32_t intervalMs = 4UL * 60UL * 60UL * 1000UL) {
  return lastSuccessMs == 0 || nowMs - lastSuccessMs >= intervalMs;
}

inline bool pairingFailureAllowsRegistration(int status) {
  return status == 409;
}

}  // namespace project_stick
