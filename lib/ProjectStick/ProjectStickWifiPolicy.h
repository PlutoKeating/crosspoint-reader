#pragma once

#include <cstdint>

// When the Wi-Fi radio is up (docs/memory-budget.md "2.7.2"). BLE-first: the
// phone relays state and delivers content, so the device rarely needs the
// cloud, and on the C3 the Wi-Fi driver (~50 KB of heap) cannot stay resident
// next to NimBLE and a content transfer. Wi-Fi is on demand: brought up for
// the few cloud jobs and the pages that need it, and powered down otherwise.
// Pure, host-tested; ProjectStickHost gathers the inputs and acts.
namespace project_stick {

struct WifiNeeds {
  bool phoneLinked = false;     // a phone holds the BLE link
  bool transferActive = false;  // a content transfer is receiving or installing
  bool bleJoinActive = false;   // a Wi-Fi join or scan asked over BLE is running
  bool joinHold = false;        // within WIFI_JOIN_HOLD_MS of a BLE join that succeeded
  bool pageHold = false;        // the Wi-Fi settings or the firmware update page is open
  bool cloudJob = false;        // a cloud job is queued or running on the worker
  bool otaRequested = false;    // a BLE `ota` request waits for the worker
  bool heartbeatDue = false;    // register due, no fresh phone sync, credential held
  bool alertWindow = false;     // credential held and inside trading hours
  bool trialPending = false;    // a trial boot or an unreported OTA outcome
};

// Pages and BLE-driven Wi-Fi work win; a running transfer loses (its heap);
// a linked phone otherwise keeps the radio down (it relays what the cloud
// would tell us); alone, the device needs the radio only for cloud jobs.
inline bool wifiWanted(const WifiNeeds& n) {
  if (n.transferActive) return false;
  if (n.bleJoinActive || n.joinHold || n.pageHold) return true;
  if (n.cloudJob || n.otaRequested) return true;
  if (n.phoneLinked) return false;
  return n.heartbeatDue || n.alertWindow || n.trialPending;
}

constexpr uint32_t WIFI_IDLE_GRACE_MS = 20000;  // unwanted this long -> power down
constexpr uint32_t WIFI_JOIN_HOLD_MS = 60000;   // the phone reads `connected` after a BLE join
constexpr uint32_t WIFI_JOB_WAIT_MS = 20000;    // a worker job waits this long for the link

// Whether to power the radio down now. Immediately when the heap is needed
// (a phone is linked or a transfer runs); after the grace otherwise, so a
// burst of short jobs does not cycle the radio.
inline bool wifiShouldPowerOff(const WifiNeeds& n, uint32_t nowMs, uint32_t lastWantedMs) {
  if (wifiWanted(n)) return false;
  if (n.transferActive || n.phoneLinked) return true;
  return nowMs - lastWantedMs >= WIFI_IDLE_GRACE_MS;
}

}  // namespace project_stick
