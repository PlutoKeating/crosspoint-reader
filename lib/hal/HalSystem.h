#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);

// Records the current heap state in RTC memory (kept across a panic reboot)
// so a crash report shows the heap shortly before the crash. Cheap; call from
// the main loop every few seconds.
void sampleHeap();
// Installs a std::new_handler that records the heap and an out-of-memory
// marker before aborting: a failed throwing `new` otherwise aborts with no
// heap information (2.2.2 crash: bad_alloc in the UI loop).
void installOutOfMemoryHandler();
bool isRebootFromPanic();
// Panic, CPU lockup, interrupt/task/RTC watchdog or brownout: the reboots
// that get a crash_report.txt (a crash screen only follows a panic).
bool isAbnormalReboot();
const char* resetReasonName();
}  // namespace HalSystem
