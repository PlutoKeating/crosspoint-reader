#include "FirmwareInstall.h"

#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

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

Candidate inspect(const char* path, const char* expectedVersion) {
  Candidate candidate;
  uint8_t head[stick_fw::IDENTIFY_BYTES];
  size_t length = 0;
  {
    HalFile file;
    if (Storage.openFileForRead("FWINST", path, file)) {
      const int read = file.read(head, sizeof(head));
      length = read > 0 ? static_cast<size_t>(read) : 0;
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

}  // namespace firmware_install
