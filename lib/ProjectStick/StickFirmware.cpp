#include "StickFirmware.h"

#include <cstring>

namespace stick_fw {

namespace {
uint32_t readU32(const uint8_t* p) {
  uint32_t value;
  memcpy(&value, p, sizeof(value));  // images are little-endian, like the target
  return value;
}

uint16_t readU16(const uint8_t* p) {
  uint16_t value;
  memcpy(&value, p, sizeof(value));
  return value;
}

// Copies a fixed-width field that must be NUL terminated within its width.
bool copyTerminated(char* dest, size_t destSize, const char* src, size_t srcSize) {
  const void* end = memchr(src, '\0', srcSize);
  if (!end) return false;
  const size_t length = static_cast<const char*>(end) - src;
  if (length >= destSize) return false;
  memcpy(dest, src, length + 1);
  return true;
}

constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr size_t CHIP_ID_OFFSET = 12;
}  // namespace

IdentifyResult identifyImage(const uint8_t* head, size_t length, ImageInfo& out) {
  out = ImageInfo{};
  if (!head || length < APP_DESC_OFFSET + 4) return IdentifyResult::TooShort;
  if (head[0] != ESP_IMAGE_MAGIC || head[1] == 0) return IdentifyResult::NotAnEspImage;
  out.chipId = readU16(head + CHIP_ID_OFFSET);
  if (readU32(head + APP_DESC_OFFSET) != ESP_APP_DESC_MAGIC) return IdentifyResult::NoAppDescriptor;
  if (length < IDENTIFY_BYTES) return IdentifyResult::TooShort;

  Descriptor desc;
  memcpy(&desc, head + DESCRIPTOR_OFFSET, sizeof(desc));
  if (desc.magic != DESCRIPTOR_MAGIC) return IdentifyResult::NotStockStick;
  if (desc.descriptorVersion != DESCRIPTOR_VERSION || desc.descriptorSize != sizeof(Descriptor)) {
    return IdentifyResult::BadDescriptor;
  }
  char product[sizeof(desc.product) + 1];
  if (!copyTerminated(product, sizeof(product), desc.product, sizeof(desc.product)) || strcmp(product, PRODUCT) != 0) {
    return IdentifyResult::NotStockStick;
  }
  if (!copyTerminated(out.version, sizeof(out.version), desc.version, sizeof(desc.version)) || out.version[0] == '\0' ||
      !copyTerminated(out.commit, sizeof(out.commit), desc.commit, sizeof(desc.commit))) {
    return IdentifyResult::BadDescriptor;
  }
  out.build = desc.build;
  out.boards = desc.boards;
  return IdentifyResult::Ok;
}

const char* identifyResultName(IdentifyResult result) {
  switch (result) {
    case IdentifyResult::Ok:
      return "OK";
    case IdentifyResult::TooShort:
      return "TOO_SHORT";
    case IdentifyResult::NotAnEspImage:
      return "NOT_ESP_IMAGE";
    case IdentifyResult::NoAppDescriptor:
      return "NO_APP_DESC";
    case IdentifyResult::NotStockStick:
      return "NOT_STOCKSTICK";
    case IdentifyResult::BadDescriptor:
      return "BAD_DESCRIPTOR";
  }
  return "?";
}

InstallVerdict checkInstall(IdentifyResult identity, const ImageInfo& image, const InstallPolicy& policy) {
  if (identity != IdentifyResult::Ok) return InstallVerdict::NotStockStick;
  if (image.chipId != policy.chipId) return InstallVerdict::WrongChip;
  if ((image.boards & policy.board) == 0) return InstallVerdict::UnsupportedBoard;
  if (image.build < policy.minimumBuild) return InstallVerdict::BelowMinimumBuild;
  if (policy.expectedVersion && strcmp(policy.expectedVersion, image.version) != 0) {
    return InstallVerdict::VersionMismatch;
  }
  return InstallVerdict::Ok;
}

const char* installVerdictName(InstallVerdict verdict) {
  switch (verdict) {
    case InstallVerdict::Ok:
      return "OK";
    case InstallVerdict::NotStockStick:
      return "NOT_STOCKSTICK_IMAGE";
    case InstallVerdict::WrongChip:
      return "WRONG_CHIP";
    case InstallVerdict::UnsupportedBoard:
      return "UNSUPPORTED_BOARD";
    case InstallVerdict::BelowMinimumBuild:
      return "BELOW_MINIMUM_BUILD";
    case InstallVerdict::VersionMismatch:
      return "VERSION_MISMATCH";
  }
  return "?";
}

BootAction decideOnBoot(TrialRecord& record, const char* runningSlot, bool abnormalReset) {
  if (!record.armed) return BootAction::None;
  if (runningSlot && strcmp(runningSlot, record.previousSlot) == 0) {
    // The bootloader (or a manual flash) went back to the old slot before the
    // new image confirmed itself.
    return BootAction::RolledBack;
  }
  if (abnormalReset && ++record.attempts >= MAX_TRIAL_FAILURES) return BootAction::RollbackNow;
  return BootAction::Continue;
}

HealthDecision evaluateHealth(const HealthInputs& inputs) {
  if (inputs.apiResponded) return HealthDecision::Confirm;
  if (inputs.transportFailures >= ONLINE_FAILURE_LIMIT &&
      inputs.onlineSinceFirstFailureMs >= ONLINE_FAILURE_WINDOW_MS) {
    return HealthDecision::Rollback;
  }
  if (!inputs.everOnline && inputs.uptimeMs >= OFFLINE_CONFIRM_MS) return HealthDecision::Confirm;
  // Online but inconclusive (occasional failures, server returning nothing):
  // never keep a trial open forever.
  if (inputs.uptimeMs >= 30 * OFFLINE_CONFIRM_MS) return HealthDecision::Confirm;
  return HealthDecision::Wait;
}

}  // namespace stick_fw
