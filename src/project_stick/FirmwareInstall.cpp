#include "FirmwareInstall.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>

#include "network/FirmwareFlasher.h"
#include "network/OtaTrial.h"
#include "platform/StickFirmwareDescriptor.h"

#ifndef STOCKSTICK_FW_MIN_INSTALL_BUILD
#define STOCKSTICK_FW_MIN_INSTALL_BUILD 0
#endif
#ifndef CONFIG_IDF_FIRMWARE_CHIP_ID
#define CONFIG_IDF_FIRMWARE_CHIP_ID 0x0005  // native simulator stands in for the ESP32-C3
#endif

namespace firmware_install {

uint32_t runningBuild() { return stick_fw::runningDescriptor().build; }

uint32_t minimumInstallBuild() { return STOCKSTICK_FW_MIN_INSTALL_BUILD; }

Candidate inspect(const char* path, const char* expectedVersion, const size_t imageLength) {
  Candidate candidate;
  uint8_t head[stick_fw::IDENTIFY_BYTES];
  size_t length = 0;
  {
    HalFile file;
    if (Storage.openFileForRead("FWINST", path, file)) {
      const int read = file.read(head, sizeof(head));
      length = read > 0 ? static_cast<size_t>(read) : 0;
      if (imageLength && length > imageLength) length = imageLength;  // bytes past the image are not its own
    }
  }
  candidate.identity = stick_fw::identifyImage(head, length, candidate.info);
  stick_fw::InstallPolicy policy;
  policy.chipId = CONFIG_IDF_FIRMWARE_CHIP_ID;
  policy.board = gpio.deviceIsX3() ? stick_fw::BOARD_X3 : stick_fw::BOARD_X4;
  policy.minimumBuild = STOCKSTICK_FW_MIN_INSTALL_BUILD;
  policy.expectedVersion = expectedVersion;
  candidate.verdict = stick_fw::checkInstall(candidate.identity, candidate.info, policy);
  LOG_INF("FWINST", "%s: %s build=%u version=%s -> %s", path, stick_fw::identifyResultName(candidate.identity),
          static_cast<unsigned>(candidate.info.build), candidate.info.version,
          stick_fw::installVerdictName(candidate.verdict));
  return candidate;
}

InstallResult installFromSd(const char* path, const char* expectedVersion, const bool permissive,
                            const ProgressFn onProgress, void* ctx, const size_t length) {
  InstallResult result;
  // A trial image rolls back to the other slot; flashing it now would
  // overwrite the image the device returns to.
  if (ota_trial::active() && !permissive) {
    result.error = "trial_active";
    return result;
  }
  const Candidate candidate = inspect(path, expectedVersion, length);
  result.verdict = candidate.verdict;
  const bool stockStick = candidate.identity == stick_fw::IdentifyResult::Ok;
  if (!candidate.ok() && !permissive) {
    result.error = stick_fw::installVerdictName(candidate.verdict);
    return result;
  }
  struct Context {
    ProgressFn onProgress;
    void* ctx;
    bool stockStick;
    std::string version;
  } context{onProgress, ctx, stockStick, stockStick ? candidate.info.version : ""};
  auto progress = +[](size_t written, size_t total, void* raw) {
    auto* c = static_cast<Context*>(raw);
    if (c->onProgress) c->onProgress(written, total, c->ctx);
  };
  // Only identified StockStick images get a trial boot (rollback on crash).
  auto armTrial = +[](const esp_partition_t* dest, void* raw) {
    auto* c = static_cast<Context*>(raw);
    return !c->stockStick || ota_trial::arm(dest, c->version.c_str());
  };
  // Re-validates the image file at flash time: SD is removable.
  const auto flash =
      firmware_flash::flashFromSdPath(path, progress, &context, /*alreadyValidated=*/false, armTrial, length);
  if (flash != firmware_flash::Result::OK) {
    ota_trial::disarm();  // the new image will not boot, so there is no trial
    result.error = firmware_flash::resultName(flash);
    result.flashFailed = true;
    LOG_ERR("FWINST", "%s: flash failed: %s", path, result.error);
    return result;
  }
  result.ok = true;
  LOG_INF("FWINST", "%s installed (%s)", path, stockStick ? context.version.c_str() : "unidentified image");
  return result;
}

}  // namespace firmware_install
