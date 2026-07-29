#pragma once

#include <cstdint>

namespace project_stick {

enum class SyncResult { Updated, Unchanged, OfflineCache, NoContent, Failed, Inactive };

struct SyncReport {
  SyncResult result = SyncResult::Failed;
  bool registerAttempted = false;
  bool registerSucceeded = false;
  bool manifestAttempted = false;
  bool manifestCompleted = false;
};

enum class ManualRefreshMode { LocalReselect, FullCloudSync };

inline bool shouldRecordRegisterSuccess(const SyncReport& report) {
  return report.registerAttempted && report.registerSucceeded;
}

inline bool shouldRecordManifestPoll(const SyncReport& report) {
  return report.manifestAttempted && report.manifestCompleted;
}

inline bool registrationDue(uint32_t nowMs, uint32_t lastSuccessMs,
                            uint32_t intervalMs = 4UL * 60UL * 60UL * 1000UL) {
  return lastSuccessMs == 0 || nowMs - lastSuccessMs >= intervalMs;
}

inline ManualRefreshMode manualRefreshMode(bool online, uint32_t activeVersion,
                                           const SyncReport& lastReport) {
  if (!online) return ManualRefreshMode::LocalReselect;
  if (activeVersion == 0 || lastReport.result == SyncResult::Failed ||
      lastReport.result == SyncResult::NoContent) {
    return ManualRefreshMode::FullCloudSync;
  }
  return ManualRefreshMode::LocalReselect;
}

}  // namespace project_stick
