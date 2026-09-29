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
Candidate inspect(const char* path, const char* expectedVersion);

uint32_t runningBuild();
uint32_t minimumInstallBuild();

}  // namespace firmware_install
