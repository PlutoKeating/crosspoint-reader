#pragma once

#include <esp_partition.h>

#include <cstddef>
#include <cstdint>

// Trial boot and rollback for newly installed firmware (see StickFirmware.h
// for the policy). State lives in NVS namespace "stick_ota" so it survives
// resets, deep sleep and power loss.
//
// Works with or without bootloader rollback support: the firmware keeps its
// own attempt counter and switches otadata back to the previous slot itself,
// and additionally defers esp_ota_mark_app_valid_cancel_rollback() until the
// image is confirmed, so an IDF bootloader with rollback enabled also reverts
// images that crash before this code runs.
namespace ota_trial {

// Call once, early in setup() after HalSystem::begin(). Increments the trial
// attempt counter; may switch back to the previous slot and restart.
void onBoot();

// Records a trial before restarting into `target`. `commandId` (may be empty)
// ties the outcome to a server-side update command.
bool arm(const esp_partition_t* target, const char* commandId, const char* targetVersion);

bool active();

// Feed every StockStick API attempt: an HTTP status > 0, or <= 0 for a
// transport/TLS failure. Safe to call from any task.
void noteApiResult(int httpStatus, bool wifiConnected);

// Call from the main loop; confirms or rolls back when the policy decides.
void tick();

// Deep sleep is a clean, deliberate shutdown: treat it as proof of health.
void onCleanShutdown();

// Outcome of the last finished trial that has not been reported yet.
struct Outcome {
  bool pending = false;
  bool rolledBack = false;
  char commandId[40] = {};
  char version[33] = {};
  char reason[48] = {};
};
Outcome pendingOutcome();
// Cheap check (no NVS access) used by frequent polls.
bool hasPendingOutcome();
void clearOutcome();

}  // namespace ota_trial
