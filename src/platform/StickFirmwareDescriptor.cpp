#include "StickFirmwareDescriptor.h"

#ifndef STOCKSTICK_FW_BUILD
#define STOCKSTICK_FW_BUILD 0
#endif
#ifndef STOCKSTICK_FW_COMMIT
#define STOCKSTICK_FW_COMMIT "unknown"
#endif

// Placed by the ESP-IDF linker script directly after esp_app_desc_t
// (".rodata_custom_desc"); the image format relies on that position. The
// section is not KEEP()-ed and LTO would fold reads of a const object, so the
// symbol has external C linkage and is a linker root (-u in platformio.ini).
extern "C" __attribute__((section(".rodata_custom_desc"), used)) const stick_fw::Descriptor stick_firmware_descriptor;
extern "C" __attribute__((section(".rodata_custom_desc"), used))
const stick_fw::Descriptor stick_firmware_descriptor = {
    stick_fw::DESCRIPTOR_MAGIC,
    stick_fw::DESCRIPTOR_VERSION,
    sizeof(stick_fw::Descriptor),
    STOCKSTICK_FW_BUILD,
    stick_fw::BOARD_X3 | stick_fw::BOARD_X4,
    "stockstick",
    CROSSPOINT_VERSION,
    STOCKSTICK_FW_COMMIT,
    {},
};

namespace stick_fw {

const Descriptor& runningDescriptor() { return stick_firmware_descriptor; }

}  // namespace stick_fw
