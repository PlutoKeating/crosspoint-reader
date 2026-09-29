#pragma once

#include <cstddef>
#include <cstdint>

// Hardware-independent parts of the StockStick firmware lifecycle: the image
// descriptor that identifies a StockStick build, and the trial-boot policy
// that decides when a freshly installed image is confirmed or rolled back.
namespace stick_fw {

// ---------------------------------------------------------------------------
// Image descriptor
//
// Every StockStick image embeds one Descriptor in `.rodata_custom_desc`, which
// the ESP-IDF linker script places directly after esp_app_desc_t at the start
// of the first DROM segment. In the flat .bin that is a fixed file offset, so
// a candidate image can be identified from its first few hundred bytes
// without executing it.
// ---------------------------------------------------------------------------

constexpr uint32_t DESCRIPTOR_MAGIC = 0x57465353;  // bytes "SSFW"
constexpr uint16_t DESCRIPTOR_VERSION = 1;
constexpr uint32_t ESP_APP_DESC_MAGIC = 0xABCD5432;
constexpr size_t IMAGE_HEADER_BYTES = 24;
constexpr size_t SEGMENT_HEADER_BYTES = 8;
constexpr size_t ESP_APP_DESC_BYTES = 256;
constexpr size_t APP_DESC_OFFSET = IMAGE_HEADER_BYTES + SEGMENT_HEADER_BYTES;
constexpr size_t DESCRIPTOR_OFFSET = APP_DESC_OFFSET + ESP_APP_DESC_BYTES;
constexpr char PRODUCT[] = "stockstick";

// Board compatibility bits.
constexpr uint32_t BOARD_X3 = 1u << 0;
constexpr uint32_t BOARD_X4 = 1u << 1;

struct Descriptor {
  uint32_t magic;
  uint16_t descriptorVersion;
  uint16_t descriptorSize;
  uint32_t build;   // monotonic release number, compared instead of version strings
  uint32_t boards;  // BOARD_* bits this image supports
  char product[16];
  char version[32];
  char commit[16];
  uint8_t reserved[16];
};
static_assert(sizeof(Descriptor) == 96, "Descriptor layout is part of the image format");

// Header bytes of an image needed to identify it.
constexpr size_t IDENTIFY_BYTES = DESCRIPTOR_OFFSET + sizeof(Descriptor);

struct ImageInfo {
  uint32_t build = 0;
  uint32_t boards = 0;
  uint16_t chipId = 0;
  char version[33] = {};
  char commit[17] = {};
};

enum class IdentifyResult : uint8_t {
  Ok,
  TooShort,
  NotAnEspImage,
  NoAppDescriptor,
  NotStockStick,  // valid ESP app without a StockStick descriptor (e.g. upstream CrossPoint)
  BadDescriptor,
};

// Parses the first IDENTIFY_BYTES of an image.
IdentifyResult identifyImage(const uint8_t* head, size_t length, ImageInfo& out);
const char* identifyResultName(IdentifyResult result);

enum class InstallVerdict : uint8_t {
  Ok,
  NotStockStick,
  WrongChip,
  UnsupportedBoard,
  BelowMinimumBuild,  // would downgrade below the oldest build with this OTA/rollback stack
  VersionMismatch,    // catalogue says one version, the image contains another
};

struct InstallPolicy {
  uint16_t chipId = 0;          // running chip (esp_chip_id_t)
  uint32_t board = 0;           // running board, one BOARD_* bit
  uint32_t minimumBuild = 0;    // lowest build this firmware will install
  const char* expectedVersion;  // catalogue version, or nullptr when not known (SD card)
};

InstallVerdict checkInstall(IdentifyResult identity, const ImageInfo& image, const InstallPolicy& policy);
const char* installVerdictName(InstallVerdict verdict);

// ---------------------------------------------------------------------------
// Trial boot
//
// Before rebooting into a new image the installer arms a trial record in NVS.
// Each abnormal reset of the new image (panic or watchdog) counts as a
// failed attempt; clean resets such as deep-sleep wake-ups do not. The image
// is confirmed once it proves it can run and reach the StockStick API, or
// shuts down cleanly into deep sleep. Too many failed attempts, or the
// bootloader returning to the previous slot, ends the trial as a rollback that
// is reported to the server on the next sync.
// ---------------------------------------------------------------------------

constexpr uint8_t MAX_TRIAL_FAILURES = 3;

enum class BootAction : uint8_t {
  None,         // no trial armed
  Continue,     // trial boot, keep running and wait for health
  RolledBack,   // running the previous slot again: record rollback
  RollbackNow,  // attempts exhausted: switch to the previous slot and restart
};

struct TrialRecord {
  bool armed = false;
  uint8_t attempts = 0;        // abnormal resets of the new image so far
  char previousSlot[17] = {};  // partition label that was running before the install
  char targetSlot[17] = {};    // partition label the new image was written to
};

// `runningSlot` is the label of the partition this boot runs from and
// `abnormalReset` whether the previous run ended in a panic or watchdog. Updates `record.attempts`.
BootAction decideOnBoot(TrialRecord& record, const char* runningSlot, bool abnormalReset);

// Health policy for a trial boot. Any HTTP response from the StockStick API
// proves Wi-Fi, DNS, TLS and the cloud client work, so the build is confirmed.
// Transport failures while Wi-Fi is connected count against it; without Wi-Fi
// the build is confirmed after a grace period because nothing else can judge it.
constexpr uint32_t OFFLINE_CONFIRM_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t ONLINE_FAILURE_WINDOW_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t ONLINE_FAILURE_LIMIT = 5;

enum class HealthDecision : uint8_t { Wait, Confirm, Rollback };

struct HealthInputs {
  uint32_t uptimeMs = 0;
  bool apiResponded = false;      // at least one HTTP status from the API this boot
  uint8_t transportFailures = 0;  // API attempts that failed below HTTP while Wi-Fi was up
  uint32_t onlineSinceFirstFailureMs = 0;
  bool everOnline = false;
};

HealthDecision evaluateHealth(const HealthInputs& inputs);

}  // namespace stick_fw
