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

enum class ManualRefreshStep : uint8_t { LocalRefresh, QueueCloudSync };

struct ManualRefreshPlan {
  ManualRefreshStep steps[2] = {ManualRefreshStep::LocalRefresh,
                                ManualRefreshStep::QueueCloudSync};
  uint8_t count = 1;
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

inline bool registrationDue(uint32_t nowMs, uint32_t lastSuccessMs,
                            uint32_t intervalMs = 4UL * 60UL * 60UL * 1000UL) {
  return lastSuccessMs == 0 || nowMs - lastSuccessMs >= intervalMs;
}

inline ManualRefreshPlan manualRefreshPlan(bool online, bool cloudSyncPending,
                                           bool cloudSyncInProgress) {
  ManualRefreshPlan plan;
  if (online && !cloudSyncPending && !cloudSyncInProgress) plan.count = 2;
  return plan;
}

}  // namespace project_stick
