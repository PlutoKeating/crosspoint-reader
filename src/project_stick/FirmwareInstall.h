#pragma once

#include <StickFirmware.h>

// Pre-install checks shared by cloud OTA and the SD-card updater: identify a
// candidate image from its header and apply the install policy for this
// device (chip, board, minimum build, expected catalogue version).
namespace firmware_install {

struct Candidate {
  stick_fw::IdentifyResult identity = stick_fw::IdentifyResult::TooShort;
  stick_fw::ImageInfo info;
  stick_fw::InstallVerdict verdict = stick_fw::InstallVerdict::NotStockStick;
  bool ok() const { return verdict == stick_fw::InstallVerdict::Ok; }
};

// `expectedVersion` is the catalogue version for cloud installs, or nullptr.
Candidate inspect(const char* path, const char* expectedVersion, size_t imageLength = 0);

uint32_t runningBuild();
uint32_t minimumInstallBuild();

// The SD-card install shared by every update route (Settings > SD-card
// update, the on-device Wi-Fi download, firmware over BLE): identify the image
// under the install policy, flash it to the next OTA slot and arm the trial
// boot for StockStick images. Does not restart; the caller does once it has
// shown the result. `permissive` (recovery mode) also flashes images that are
// not identified StockStick builds, without a trial boot.
struct InstallResult {
  bool ok = false;
  stick_fw::InstallVerdict verdict = stick_fw::InstallVerdict::NotStockStick;
  // Name of the failure ("" when ok): an install verdict or a flash result.
  const char* error = "";
  // Set when the flash itself failed (as opposed to the image check).
  bool flashFailed = false;
};
using ProgressFn = void (*)(size_t written, size_t total, void* ctx);
// `length`: the image's bytes at the start of `path` (the firmware area,
// 2.7.10); 0 for the whole file (an SD-card .bin, unchanged).
InstallResult installFromSd(const char* path, const char* expectedVersion, bool permissive, ProgressFn onProgress,
                            void* ctx, size_t length = 0);

}  // namespace firmware_install
