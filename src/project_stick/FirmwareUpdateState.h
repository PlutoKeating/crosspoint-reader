#pragma once

#include <cstdint>

// Progress of a running firmware update, published by the background sync
// task and read by the UI. Plain values under a spinlock; no allocation.
namespace firmware_update {

enum class Phase : uint8_t { Idle, Downloading, Verifying, Installing, Restarting, Failed };

struct Snapshot {
  Phase phase = Phase::Idle;
  uint32_t done = 0;
  uint32_t total = 0;
  uint32_t generation = 0;  // changes on every update; lets the UI redraw only when needed
  char version[33] = {};
  char error[48] = {};
  uint8_t percent() const { return total ? static_cast<uint8_t>(static_cast<uint64_t>(done) * 100 / total) : 0; }
  bool busy() const { return phase != Phase::Idle && phase != Phase::Failed; }
};

void begin(const char* version, uint32_t total);
void setPhase(Phase phase);
// Publishes progress only when the whole percentage changes, so e-ink redraws stay rare.
void setProgress(uint32_t done);
void fail(const char* error);
void reset();
Snapshot snapshot();

// Whether external power is charging the battery, published by the main loop
// (which owns the fuel gauge's I2C bus) for the install guard on the worker.
void setExternalPower(bool charging);
bool externalPower();

}  // namespace firmware_update
