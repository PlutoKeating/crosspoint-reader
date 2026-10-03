#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Trial boot and rollback for newly installed firmware (see StickFirmware.h
// for the policy). State lives in NVS namespace "stick_ota" so it survives
// resets, deep sleep and power loss.
//
// Works with or without bootloader rollback support: the firmware keeps its
// own attempt counter and switches otadata back to the previous slot itself.
// verifyRollbackLater() keeps the image PENDING_VERIFY until onBoot(), so an
// IDF bootloader with rollback enabled still reverts images that crash before
// onBoot() runs; from there on the app-level counter decides.
namespace ota_trial {

// Call once, early in setup() after HalSystem::begin(). Increments the trial
// attempt counter; may switch back to the previous slot and restart.
void onBoot();

// Records a trial before restarting into `target`.
bool arm(const esp_partition_t* target, const char* targetVersion);

bool active();

// Clears an armed record whose install never switched boot slots (the new
// image will not run, so there is no trial to judge). No-op during a trial.
void disarm();

// Feed every StockStick API attempt: an HTTP status > 0, or <= 0 for a
// transport/TLS failure. Safe to call from any task.
void noteApiResult(int httpStatus, bool wifiConnected);

// A phone completed an authenticated BLE sync (2.6.0): BLE, storage and the
// store work, which confirms the image like an API response would.
void notePhoneSync();

// Call from the main loop; confirms or rolls back when the policy decides.
void tick(bool wifiConnected);

// Deep sleep is a clean, deliberate shutdown: treat it as proof of health.
void onCleanShutdown();

// Outcome of the last finished trial that has not been reported yet.
struct Outcome {
  bool pending = false;
  bool rolledBack = false;
  char version[33] = {};
  char reason[48] = {};
};
Outcome pendingOutcome();
// Cheap check (no NVS access) used by frequent polls.
bool hasPendingOutcome();
void clearOutcome();

}  // namespace ota_trial
