#pragma once

#include <StickFirmware.h>

// Identity of the running image. The same bytes are embedded in the flashed
// .bin (see StickFirmware.h), so tools and the OTA installer read them from a
// candidate image without executing it.
namespace stick_fw {
const Descriptor& runningDescriptor();
}  // namespace stick_fw
