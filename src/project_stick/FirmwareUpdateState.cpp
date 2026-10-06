#include "FirmwareUpdateState.h"

#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>

namespace firmware_update {

namespace {
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
Snapshot state;
std::atomic<bool> charging{false};

void copy(char* dest, size_t size, const char* src) {
  strncpy(dest, src ? src : "", size - 1);
  dest[size - 1] = '\0';
}
}  // namespace

void begin(const char* version, uint32_t total) {
  taskENTER_CRITICAL(&stateMux);
  state.phase = Phase::Downloading;
  state.done = 0;
  state.total = total;
  copy(state.version, sizeof(state.version), version);
  state.error[0] = '\0';
  ++state.generation;
  taskEXIT_CRITICAL(&stateMux);
}

void setPhase(Phase phase) {
  taskENTER_CRITICAL(&stateMux);
  if (state.phase != phase) {
    state.phase = phase;
    if (phase == Phase::Installing) state.done = 0;
    ++state.generation;
  }
  taskEXIT_CRITICAL(&stateMux);
}

void setProgress(uint32_t done) {
  taskENTER_CRITICAL(&stateMux);
  const uint8_t before = state.percent();
  state.done = done;
  if (state.percent() != before) ++state.generation;
  taskEXIT_CRITICAL(&stateMux);
}

void fail(const char* error) {
  taskENTER_CRITICAL(&stateMux);
  state.phase = Phase::Failed;
  copy(state.error, sizeof(state.error), error);
  ++state.generation;
  taskEXIT_CRITICAL(&stateMux);
}

void reset() {
  taskENTER_CRITICAL(&stateMux);
  state.phase = Phase::Idle;
  state.done = state.total = 0;
  state.version[0] = state.error[0] = '\0';
  ++state.generation;
  taskEXIT_CRITICAL(&stateMux);
}

Snapshot snapshot() {
  taskENTER_CRITICAL(&stateMux);
  const Snapshot copyOfState = state;
  taskEXIT_CRITICAL(&stateMux);
  return copyOfState;
}

void setExternalPower(const bool value) { charging.store(value, std::memory_order_relaxed); }
bool externalPower() { return charging.load(std::memory_order_relaxed); }

}  // namespace firmware_update
