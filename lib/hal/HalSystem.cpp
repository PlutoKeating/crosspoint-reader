#include "HalSystem.h"

#include <new>
#include <string>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];

namespace {
struct HeapSample {
  uint32_t magic;
  uint32_t freeBytes, minFree, maxAlloc, uptimeMs;
  uint32_t outOfMemory;  // 1 when a throwing operator new failed
};
constexpr uint32_t HEAP_SAMPLE_MAGIC = 0x48454150;  // "HEAP"
}  // namespace
RTC_NOINIT_ATTR HeapSample panicHeap;
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
namespace {
// A CPU exception never passes through panic_abort(), so without this the
// report has an empty reason; the frame registers and the faulting task name
// make such a crash diagnosable (2.4.3: stack overflow in nimble_host).
struct ExceptionRecord {
  uint32_t magic;
  uint32_t mcause, mtval, mepc, ra, sp;
  uint32_t stackFree;  // high-water mark of the faulting task, bytes
  char task[configMAX_TASK_NAME_LEN + 1];
};
constexpr uint32_t EXCEPTION_MAGIC = 0x45584350;  // "EXCP"
}  // namespace
RTC_NOINIT_ATTR ExceptionRecord panicException;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  const auto* exc = static_cast<const RvExcFrame*>(frame);
  panicException.mcause = exc->mcause;
  panicException.mtval = exc->mtval;
  panicException.mepc = exc->mepc;
  panicException.ra = exc->ra;
  panicException.sp = exc->sp;
  panicException.stackFree = 0;
  panicException.task[0] = '\0';
  if (TaskHandle_t current = xTaskGetCurrentTaskHandle()) {
    const char* name = pcTaskGetName(current);
    int i = 0;
    for (; name && i < (int)sizeof(panicException.task) - 1 && name[i]; i++) panicException.task[i] = name[i];
    panicException.task[i] = '\0';
    panicException.stackFree = uxTaskGetStackHighWaterMark(current);
  }
  panicException.magic = EXCEPTION_MAGIC;

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void sampleHeap() {
  panicHeap.freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  panicHeap.minFree = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  panicHeap.maxAlloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  panicHeap.uptimeMs = millis();
  panicHeap.magic = HEAP_SAMPLE_MAGIC;
}

namespace {
void outOfMemory() {
  sampleHeap();
  panicHeap.outOfMemory = 1;
  LOG_ERR("SYS", "Out of memory: operator new failed (free=%u min=%u max=%u)", (unsigned)panicHeap.freeBytes,
          (unsigned)panicHeap.minFree, (unsigned)panicHeap.maxAlloc);
  abort();
}
}  // namespace

void installOutOfMemoryHandler() { std::set_new_handler(outOfMemory); }

void clearPanic() {
  panicMessage[0] = '\0';
  panicHeap.magic = 0;
  panicException.magic = 0;
  panicHeap.outOfMemory = 0;
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;
    info += "\n\nPanic reason: " + std::string(panicMessage);
    if (panicException.magic == EXCEPTION_MAGIC) {
      static const char* const CAUSES[] = {"Instruction address misaligned",
                                           "Instruction access fault",
                                           "Illegal instruction",
                                           "Breakpoint",
                                           "Load address misaligned",
                                           "Load access fault",
                                           "Store address misaligned",
                                           "Store access fault"};
      const uint32_t cause = panicException.mcause & 0x7fffffff;
      const char* name = cause < sizeof(CAUSES) / sizeof(CAUSES[0]) ? CAUSES[cause] : "(see mcause)";
      char line[200];
      snprintf(line, sizeof(line),
               "\nException: %s (mcause=0x%x) mepc=0x%08x ra=0x%08x mtval=0x%08x sp=0x%08x task=%s stack_free=%u",
               name, (unsigned)panicException.mcause, (unsigned)panicException.mepc, (unsigned)panicException.ra,
               (unsigned)panicException.mtval, (unsigned)panicException.sp, panicException.task,
               (unsigned)panicException.stackFree);
      info += line;
    }
    if (panicHeap.magic == HEAP_SAMPLE_MAGIC) {
      char heap[160];
      snprintf(heap, sizeof(heap), "\n%sHeap at %u ms: free=%u min=%u largest=%u",
               panicHeap.outOfMemory ? "Out of memory (operator new failed). " : "", (unsigned)panicHeap.uptimeMs,
               (unsigned)panicHeap.freeBytes, (unsigned)panicHeap.minFree, (unsigned)panicHeap.maxAlloc);
      info += heap;
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP;
}

}  // namespace HalSystem
